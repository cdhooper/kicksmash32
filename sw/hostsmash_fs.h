#ifndef _HOSTSMASH_FS_H
#define _HOSTSMASH_FS_H

#define AV_FLAG_BOOTABLE 0x01

typedef struct amiga_vol  amiga_vol_t;
typedef struct handle_ent handle_ent_t;
typedef struct handle_ent {
    handle_t      he_handle;   // Reference handle for Amiga interface
    char         *he_name;     // Name of file's path relative to parent
    char         *he_path;     // Full local filesystem relative path
    int           he_fd;       // Open file number
    uint          he_type;     // One of HM_TYPE_*
    uint          he_mode;     // Open mode
    uint          he_count;    // Open count
    uint          he_entnum;   // Volume directory entry number
    DIR          *he_dir;      // Open directory pointer
    amiga_vol_t  *he_avolume;  // Volume descriptor for this handle
    handle_ent_t *he_volume;   // Volume for this file
    handle_ent_t *he_next;     // Next in list of all handles
} handle_ent_t;

typedef struct amiga_vol {
    const char   *av_volume;   // Amiga path
    const char   *av_path;     // Host path
    const char   *av_realpath; // Host real path (not relative)
    handle_ent_t *av_handle;
    amiga_vol_t  *av_next;
    uint          av_flags;
    int           av_bootpri;
} amiga_vol_t;
extern amiga_vol_t *amiga_vol_head;
extern uint debug_fs;

uint sm_fopen(hm_fopenhandle_t *hm, uint *status);
uint sm_fclose(hm_fopenhandle_t *hm, uint *status);
uint sm_fread(hm_freadwrite_t *hm, uint *status);
uint sm_fwrite(hm_freadwrite_t *hm, uint rxlen, uint *status);
uint sm_fseek(hm_fseek_t *hm, uint *status);
uint sm_fcreate(hm_fopenhandle_t *hm, uint *status);
uint sm_fdelete(hm_fhandle_t *hm, uint *status);
uint sm_frename(hm_frename_t *hm, uint *status);
uint sm_fpath(hm_fhandle_t *hm, uint *status);
uint sm_fsetdate(hm_fsetdate_t *hm, uint *status);
uint sm_fsetown(hm_fsetown_t *hm, uint *status);
uint sm_fsetprotect(hm_fopenhandle_t *hm, uint *status);
void volume_add(const char *volume_name, const char *local_path,
                uint is_default);

#endif /* _HOSTSMASH_FS_H */
