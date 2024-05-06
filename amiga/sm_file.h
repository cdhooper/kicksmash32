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

uint sm_open(handle_t parent_handle, const char *name, uint mode,
             uint *type, uint create_perms, handle_t *handle);
uint sm_close(handle_t handle);
uint sm_read(handle_t handle, uint readsize, void **data, uint *rlen);
uint sm_write(handle_t handle, void *buf, uint buflen);
uint sm_path(handle_t handle, char **name);
uint sm_rename(handle_t handle, const char *name_old, const char *name_new);
uint sm_create(handle_t parent_handle, const char *name,
               const char *tgt_name, uint hm_type);
uint sm_delete(handle_t handle, const char *name);
uint sm_set_perms(handle_t parent_handle, const char *name, uint perms);

const char *km_status(uint km_status);

#endif /* _SM_FILE_H */

