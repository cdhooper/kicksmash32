/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in August 2024.
 *
 * ---------------------------------------------------------------------
 *
 * Smash remote host file transfer and management functions.
 */

#ifndef _SM_FILE_H
#define _SM_FILE_H

uint sm_fservice(void);
uint sm_fopen(handle_t parent_handle, const char *name, uint mode,
              uint *hm_type, uint create_perms, handle_t *handle);
uint sm_fclose(handle_t handle);
uint sm_fread(handle_t handle, uint readsize, void **data, uint *rlen,
              uint flags);
uint sm_fwrite(handle_t handle, void *buf, uint writelen, uint padded_header,
               uint flags);
uint sm_fpath(handle_t handle, char **name);
uint sm_frename(handle_t shandle, const char *name_old,
                handle_t dhandle, const char *name_new);
uint sm_fcreate(handle_t parent_handle, const char *name, const char *tgt_name,
                uint hm_type, uint create_perms);
uint sm_fdelete(handle_t handle, const char *name);
uint sm_fseek(handle_t handle, int seek_mode, uint64_t offset,
              uint64_t *new_pos, uint64_t *prev_pos);
uint sm_fsetdate(handle_t parent_handle, const char *name, uint which,
                 uint *sec, uint *nsec);
uint sm_fsetown(handle_t parent_handle, const char *name, uint oid, uint gid);
uint sm_fsetprotect(handle_t parent_handle, const char *name, uint perms);

const char *km_status(uint km_status);

extern uint8_t sm_file_active;

#define SEEK_OFFSET_BEGINNING (-1)
#define SEEK_OFFSET_CURRENT   (0)
#define SEEK_OFFSET_END       (1)

#endif /* _SM_FILE_H */
