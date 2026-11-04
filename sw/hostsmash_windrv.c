/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2026.
 *
 * ---------------------------------------------------------------------
 *
 * Hostsmash Windows driver setup.
 *
 * Kicksmash USB devices need a working Windows driver before hostsmash
 * can talk to them:
 *
 * - DFU devices (the Kicksmash firmware updater and the STM32 and GD32
 *   ROM DFU bootloaders) are accessed with libusb, which on Windows can
 *   only use devices bound to WinUSB. Current updaters provide Microsoft
 *   OS 2.0 descriptors so that Windows binds WinUSB automatically, but
 *   older updaters and the ROM bootloaders do not, and are left with no
 *   driver at all.
 *
 * - The Kicksmash CDC ACM serial device (1209:1610). Firmware which does
 *   not provide an Interface Association Descriptor is split by Windows
 *   into separate interface devices, and Windows' built-in usbser driver
 *   then fails to start (code 10), so no COM port is available.
 *
 * Both are fixed by assigning one of Windows' own signed in-box drivers
 * to the device: winusb.inf ("WinUsb Device") for DFU devices, or
 * usbser.inf ("USB Serial Device", bound to the whole device) for the
 * serial device. This is the same as manually picking the driver from
 * the list in Device Manager, and requires no external program or driver
 * package. It does require administrator rights, so if hostsmash isn't
 * elevated, it runs an elevated copy of itself (which shows a User
 * Account Control prompt) to perform the installation.
 */

#ifdef _WIN32

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <shellapi.h>
#include "hostsmash_windrv.h"

#define WINDRV_OPT         "--install-driver"  // Internal: elevated install
#define WINDRV_INST_MAX    256                 // Device instance ID length
#define WINDRV_TRIED_MAX   8                   // Devices attempted per run

#define KS_SERIAL_PARENT   "USB\\VID_1209&PID_1610\\"
#define KS_SERIAL_IFACE0   "USB\\VID_1209&PID_1610&MI_00\\"

/* DiInstallDevice() only exists as a wide character (W) function */
typedef BOOL (WINAPI *di_install_device_t)(HWND, HDEVINFO, PSP_DEVINFO_DATA,
                                           PSP_DRVINFO_DATA_W, DWORD, PBOOL);

/* Device interface GUID registered for DFU devices given WinUSB */
static const char windrv_winusb_ifguid[] =
    "{B53EAB80-04C3-4486-AAFB-7393460808A0}\0";

/* Device instances already attempted, so each is tried only once */
static char         windrv_tried[WINDRV_TRIED_MAX][WINDRV_INST_MAX];
static unsigned int windrv_ntried = 0;

/*
 * windrv_err() displays a message with the Windows error description.
 */
static void
windrv_err(const char *what)
{
    DWORD err = GetLastError();
    char  msg[256];

    if (FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS, NULL, err, 0,
                       msg, sizeof (msg), NULL) == 0) {
        msg[0] = '\0';
    }
    msg[strcspn(msg, "\r\n")] = '\0';
    printf("%s failed: 0x%lx %s\n", what, (unsigned long) err, msg);
}

/*
 * windrv_prefix() returns true if the device instance ID begins with the
 *                 specified prefix (case-insensitive).
 */
static bool
windrv_prefix(const char *inst, const char *prefix)
{
    return (_strnicmp(inst, prefix, strlen(prefix)) == 0);
}

/*
 * windrv_tried_before() returns true if a driver install was already
 *                       attempted for the device instance.
 */
static bool
windrv_tried_before(const char *inst)
{
    unsigned int cur;

    for (cur = 0; cur < windrv_ntried; cur++)
        if (_stricmp(inst, windrv_tried[cur]) == 0)
            return (true);
    return (false);
}

/*
 * windrv_dev_state() gets the service (driver) name of a device and
 *                    whether it is started without a problem.
 */
static void
windrv_dev_state(HDEVINFO set, SP_DEVINFO_DATA *info, char *service,
                 size_t service_len, bool *working)
{
    ULONG status;
    ULONG problem;

    if (!SetupDiGetDeviceRegistryPropertyA(set, info, SPDRP_SERVICE, NULL,
                                           (PBYTE) service, service_len,
                                           NULL)) {
        service[0] = '\0';
    }
    *working = (CM_Get_DevNode_Status(&status, &problem, info->DevInst, 0) ==
                CR_SUCCESS) && ((status & DN_STARTED) != 0) &&
               ((status & DN_HAS_PROBLEM) == 0);
}

/*
 * windrv_guid_str() formats a GUID in registry form, for example
 *                   {4D36E978-E325-11CE-BFC1-08002BE10318}.
 */
static void
windrv_guid_str(const GUID *guid, char *buf, size_t buflen)
{
    snprintf(buf, buflen,
             "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
             (unsigned long) guid->Data1, guid->Data2, guid->Data3,
             guid->Data4[0], guid->Data4[1], guid->Data4[2], guid->Data4[3],
             guid->Data4[4], guid->Data4[5], guid->Data4[6], guid->Data4[7]);
}

/*
 * windrv_install_local() assigns a driver from one of Windows' in-box INF
 *                        files to the specified device instance.
 *                        Administrator rights are required.
 *
 * @param [in] inst    - Device instance ID.
 * @param [in] inf     - INF file name in %WINDIR%\INF.
 * @param [in] section - Install section of the driver to select, or NULL
 *                       to select the first driver in the INF file.
 *
 * @return 0 - Success.
 * @return 1 - Failure (a message was displayed).
 */
static int
windrv_install_local(const char *inst, const char *inf, const char *section)
{
    SP_DEVINSTALL_PARAMS_A params;
    SP_DEVINFO_DATA        dev;
    SP_DRVINFO_DATA_W      drv;
    di_install_device_t    di_install;
    HDEVINFO               set;
    DWORD                  index;
    DWORD                  old_class_len = 0;
    BOOL                   reboot = FALSE;
    GUID                   inf_class;
    bool                   found = false;
    bool                   class_changed = false;
    char                   windir[MAX_PATH];
    char                   inf_class_name[64];
    char                   old_class[40];
    char                   new_class[40];
    int                    rc = 1;

    /* The device may have no class until a driver is installed */
    set = SetupDiCreateDeviceInfoList(NULL, NULL);
    if (set == INVALID_HANDLE_VALUE) {
        windrv_err("SetupDiCreateDeviceInfoList");
        return (1);
    }
    dev.cbSize = sizeof (dev);
    if (!SetupDiOpenDeviceInfoA(set, inst, NULL, 0, &dev)) {
        windrv_err("SetupDiOpenDeviceInfo");
        goto fail;
    }

    /*
     * Build a driver list from only the specified INF file, including
     * drivers which don't match the device and those which are excluded
     * from selection (as Device Manager does when picking a driver).
     */
    params.cbSize = sizeof (params);
    if (!SetupDiGetDeviceInstallParamsA(set, &dev, &params)) {
        windrv_err("SetupDiGetDeviceInstallParams");
        goto fail;
    }
    if ((GetWindowsDirectoryA(windir, sizeof (windir)) == 0) ||
        (snprintf(params.DriverPath, sizeof (params.DriverPath),
                  "%s\\INF\\%s", windir, inf) >=
         (int) sizeof (params.DriverPath))) {
        printf("Could not locate %s\n", inf);
        goto fail;
    }

    /*
     * The class driver list only includes drivers of the device's own
     * setup class, so the device must first be given the INF's class
     * (as Device Manager does when a driver of another class is picked).
     * For example, a composite device is class USB, but usbser.inf
     * drivers are class Ports.
     */
    if (!SetupDiGetINFClassA(params.DriverPath, &inf_class, inf_class_name,
                             sizeof (inf_class_name), NULL)) {
        windrv_err("SetupDiGetINFClass");
        goto fail;
    }
    if (memcmp(&dev.ClassGuid, &inf_class, sizeof (inf_class)) != 0) {
        windrv_guid_str(&inf_class, new_class, sizeof (new_class));
        if (!SetupDiGetDeviceRegistryPropertyA(set, &dev, SPDRP_CLASSGUID,
                                               NULL, (PBYTE) old_class,
                                               sizeof (old_class),
                                               &old_class_len)) {
            old_class_len = 0;  // Device has no class
        }
        if (!SetupDiSetDeviceRegistryPropertyA(set, &dev, SPDRP_CLASSGUID,
                                               (const BYTE *) new_class,
                                               strlen(new_class) + 1)) {
            windrv_err("Setting device class");
            goto fail;
        }
        class_changed = true;
    }

    params.Flags   |= DI_ENUMSINGLEINF;
    params.FlagsEx |= DI_FLAGSEX_ALLOWEXCLUDEDDRVS;
    if (!SetupDiSetDeviceInstallParamsA(set, &dev, &params)) {
        windrv_err("SetupDiSetDeviceInstallParams");
        goto fail;
    }
    if (!SetupDiBuildDriverInfoList(set, &dev, SPDIT_CLASSDRIVER)) {
        windrv_err("SetupDiBuildDriverInfoList");
        goto fail;
    }
    drv.cbSize = sizeof (drv);
    for (index = 0;
         SetupDiEnumDriverInfoW(set, &dev, SPDIT_CLASSDRIVER, index, &drv);
         index++) {
        SP_DRVINFO_DETAIL_DATA_W detail;
        wchar_t                  wsection[LINE_LEN];

        if (section == NULL) {
            found = true;
            break;
        }
        detail.cbSize = sizeof (detail);
        if (!SetupDiGetDriverInfoDetailW(set, &dev, &drv, &detail,
                                         sizeof (detail), NULL) &&
            (GetLastError() != ERROR_INSUFFICIENT_BUFFER)) {
            continue;
        }
        if (MultiByteToWideChar(CP_ACP, 0, section, -1, wsection,
                                LINE_LEN) == 0) {
            continue;
        }
        if (_wcsicmp(detail.SectionName, wsection) == 0) {
            found = true;
            break;
        }
    }
    if (!found) {
        printf("No %s driver found in %s\n",
               (section != NULL) ? section : "", inf);
        goto fail;
    }
    if (!SetupDiSetSelectedDriverW(set, &dev, &drv)) {
        windrv_err("SetupDiSetSelectedDriver");
        goto fail;
    }

    if (_stricmp(inf, "winusb.inf") == 0) {
        /*
         * Give WinUSB a device interface GUID to register. libusb doesn't
         * require it, but other WinUSB applications look devices up by it.
         */
        HKEY key = SetupDiCreateDevRegKeyA(set, &dev, DICS_FLAG_GLOBAL, 0,
                                           DIREG_DEV, NULL, NULL);
        if (key != INVALID_HANDLE_VALUE) {
            RegSetValueExA(key, "DeviceInterfaceGUIDs", 0, REG_MULTI_SZ,
                           (const BYTE *) windrv_winusb_ifguid,
                           sizeof (windrv_winusb_ifguid));
            RegCloseKey(key);
        }
    }

    /* MinGW's libnewdev.a lacks DiInstallDevice(), so look it up */
    di_install = (di_install_device_t) (void (*)(void))
                 GetProcAddress(LoadLibraryA("newdev.dll"), "DiInstallDevice");
    if (di_install == NULL) {
        windrv_err("Locating DiInstallDevice in newdev.dll");
        goto fail;
    }
    if (!di_install(NULL, set, &dev, &drv, 0, &reboot)) {
        windrv_err("DiInstallDevice");
        goto fail;
    }
    if (reboot)
        printf("Windows reports a reboot is required to use the driver\n");
    rc = 0;
fail:
    if ((rc != 0) && class_changed) {
        /* Put the device's original class back */
        SetupDiSetDeviceRegistryPropertyA(set, &dev, SPDRP_CLASSGUID,
                                          (old_class_len != 0) ?
                                          (const BYTE *) old_class : NULL,
                                          old_class_len);
    }
    SetupDiDestroyDeviceInfoList(set);
    return (rc);
}

/*
 * windrv_elevated() returns true if this process has administrator rights.
 */
static bool
windrv_elevated(void)
{
    TOKEN_ELEVATION elev;
    HANDLE          token;
    DWORD           len;
    bool            rc = false;

    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        if (GetTokenInformation(token, TokenElevation, &elev, sizeof (elev),
                                &len)) {
            rc = (elev.TokenIsElevated != 0);
        }
        CloseHandle(token);
    }
    return (rc);
}

/*
 * windrv_run_elevated() runs an elevated copy of hostsmash with the
 *                       specified internal option arguments. Windows
 *                       displays a User Account Control prompt.
 *
 * @return 0 - Success.
 * @return 1 - Failure.
 */
static int
windrv_run_elevated(const char *args, const char *what)
{
    SHELLEXECUTEINFOA sei;
    DWORD             code = 1;
    char              exe[MAX_PATH];

    if (GetModuleFileNameA(NULL, exe, sizeof (exe)) == 0)
        return (1);
    memset(&sei, 0, sizeof (sei));
    sei.cbSize       = sizeof (sei);
    sei.fMask        = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb       = "runas";
    sei.lpFile       = exe;
    sei.lpParameters = args;
    sei.nShow        = SW_HIDE;
    printf("Approve the Windows administrator prompt to %s\n", what);
    if (!ShellExecuteExA(&sei) || (sei.hProcess == NULL)) {
        windrv_err("Elevated operation");
        return (1);
    }
    WaitForSingleObject(sei.hProcess, INFINITE);
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return ((code == 0) ? 0 : 1);
}

/*
 * windrv_install() installs a driver for the device instance, using an
 *                  elevated copy of hostsmash if necessary. Each device
 *                  instance is attempted at most once.
 *
 * @return true  - The driver was installed.
 * @return false - The install failed or was already attempted.
 */
static bool
windrv_install(const char *inst, const char *inf, const char *section,
               const char *why)
{
    int rc;

    if (windrv_tried_before(inst) || (windrv_ntried >= WINDRV_TRIED_MAX))
        return (false);
    strcpy(windrv_tried[windrv_ntried++], inst);

    printf("%s\nInstalling Windows %s driver for %s\n", why, inf, inst);
    if (windrv_elevated()) {
        rc = windrv_install_local(inst, inf, section);
    } else {
        char args[400];
        if (snprintf(args, sizeof (args), "%s %s %s \"%s\"", WINDRV_OPT,
                     inf, (section != NULL) ? section : "-", inst) >=
            (int) sizeof (args)) {
            return (false);
        }
        rc = windrv_run_elevated(args, "install the driver");
    }
    if (rc != 0) {
        printf("Driver install failed\n");
        return (false);
    }
    return (true);
}

/*
 * windrv_internal_main() handles the internal command line options used
 *                        by the elevated copy of hostsmash:
 *                            --install-driver <inf> <section|-> <instance>
 *
 * @return -1 - argv is not an internal option.
 * @return  0 - Success.
 * @return  1 - Failure.
 */
int
windrv_internal_main(int argc, char * const *argv)
{
    if ((argc == 5) && (strcmp(argv[1], WINDRV_OPT) == 0)) {
        return (windrv_install_local(argv[4], argv[2],
                                     (strcmp(argv[3], "-") == 0) ? NULL :
                                     argv[3]));
    }
    return (-1);
}

/*
 * windrv_fix_dfu() assigns the WinUSB driver to each attached supported
 *                  DFU device which has no driver.
 *
 * @return true  - A driver was installed.
 * @return false - Nothing was installed.
 */
bool
windrv_fix_dfu(void)
{
    static const char * const hwids[] = {
        "USB\\VID_0483&PID_DF11\\", "USB\\VID_28E9&PID_0189\\"
    };
    SP_DEVINFO_DATA info;
    HDEVINFO        set;
    DWORD           index;
    char            todo[WINDRV_TRIED_MAX][WINDRV_INST_MAX];
    unsigned int    ntodo = 0;
    unsigned int    cur;
    bool            installed = false;

    set = SetupDiGetClassDevsA(NULL, "USB", NULL,
                               DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE)
        return (false);
    info.cbSize = sizeof (info);
    for (index = 0; (ntodo < WINDRV_TRIED_MAX) &&
                    SetupDiEnumDeviceInfo(set, index, &info); index++) {
        char inst[WINDRV_INST_MAX];
        char service[64];
        bool working;

        if (!SetupDiGetDeviceInstanceIdA(set, &info, inst, sizeof (inst),
                                         NULL)) {
            continue;
        }
        for (cur = 0; cur < sizeof (hwids) / sizeof (hwids[0]); cur++)
            if (windrv_prefix(inst, hwids[cur]))
                break;
        if (cur == sizeof (hwids) / sizeof (hwids[0]))
            continue;
        windrv_dev_state(set, &info, service, sizeof (service), &working);
        if (service[0] != '\0')
            continue;  // Has a driver
        strcpy(todo[ntodo++], inst);
    }
    SetupDiDestroyDeviceInfoList(set);

    for (cur = 0; cur < ntodo; cur++) {
        if (windrv_install(todo[cur], "winusb.inf", NULL,
                           "DFU device has no Windows driver."))
            installed = true;
    }
    return (installed);
}

/*
 * windrv_fix_kicksmash() assigns Windows' built-in USB serial driver to
 *                        each attached Kicksmash which does not have a
 *                        working COM port. This happens with Kicksmash
 *                        firmware which has no Interface Association
 *                        Descriptor, when no other driver (such as one
 *                        installed by wdikicksmash.bat or Zadig) is bound.
 *
 * @return true  - A driver was installed.
 * @return false - Nothing was installed.
 */
bool
windrv_fix_kicksmash(void)
{
    struct {
        char    inst[WINDRV_INST_MAX];
        DEVINST devinst;
        bool    working;    // Has a working COM port
    } parents[WINDRV_TRIED_MAX];
    struct {
        DEVINST parent;
        bool    working;
    } ifaces[WINDRV_TRIED_MAX];
    SP_DEVINFO_DATA info;
    HDEVINFO        set;
    DWORD           index;
    unsigned int    nparents = 0;
    unsigned int    nifaces = 0;
    unsigned int    cur;
    unsigned int    pos;
    bool            installed = false;

    set = SetupDiGetClassDevsA(NULL, "USB", NULL,
                               DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE)
        return (false);
    info.cbSize = sizeof (info);
    for (index = 0; SetupDiEnumDeviceInfo(set, index, &info); index++) {
        char inst[WINDRV_INST_MAX];
        char service[64];
        bool working;

        if (!SetupDiGetDeviceInstanceIdA(set, &info, inst, sizeof (inst),
                                         NULL)) {
            continue;
        }
        if (windrv_prefix(inst, KS_SERIAL_PARENT)) {
            if (nparents >= WINDRV_TRIED_MAX)
                continue;
            windrv_dev_state(set, &info, service, sizeof (service), &working);
            strcpy(parents[nparents].inst, inst);
            parents[nparents].devinst = info.DevInst;
            /* usbser bound to the whole device (wdi driver or our fix) */
            parents[nparents].working = working &&
                                        (_stricmp(service, "usbser") == 0);
            nparents++;
        } else if (windrv_prefix(inst, KS_SERIAL_IFACE0)) {
            if ((nifaces >= WINDRV_TRIED_MAX) ||
                (CM_Get_Parent(&ifaces[nifaces].parent, info.DevInst, 0) !=
                 CR_SUCCESS)) {
                continue;
            }
            /* usbser bound to the CDC function of a composite device */
            windrv_dev_state(set, &info, service, sizeof (service), &working);
            ifaces[nifaces].working = working &&
                                      (_stricmp(service, "usbser") == 0);
            nifaces++;
        }
    }
    SetupDiDestroyDeviceInfoList(set);

    for (cur = 0; cur < nparents; cur++) {
        for (pos = 0; pos < nifaces; pos++)
            if ((ifaces[pos].parent == parents[cur].devinst) &&
                ifaces[pos].working)
                parents[cur].working = true;
        if (parents[cur].working)
            continue;
        if (windrv_install(parents[cur].inst, "usbser.inf",
                           "UsbSerial_Install",
                           "Kicksmash has no working Windows COM port.")) {
            installed = true;
        }
    }
    return (installed);
}

#else
typedef int hostsmash_windrv_unused;  // ISO C forbids an empty file
#endif /* _WIN32 */
