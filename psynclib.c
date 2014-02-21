/* Copyright (c) 2013 Anton Titov.
 * Copyright (c) 2013 pCloud Ltd.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of pCloud Ltd nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL pCloud Ltd BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "plibs.h"
#include "pcompat.h"
#include "psynclib.h"
#include "pcallbacks.h"
#include "pdatabase.h"
#include "pstatus.h"
#include "pdiff.h"
#include "pssl.h"
#include "ptimer.h"
#include "pupload.h"
#include "pdownload.h"
#include "pfolder.h"
#include "psettings.h"
#include "psyncer.h"
#include "ptasks.h"
#include "papi.h"
#include "pnetlibs.h"
#include "pscanner.h"
#include "plocalscan.h"
#include "plist.h"
#include "pp2p.h"
#include <string.h>
#include <ctype.h>
#include <stddef.h>

typedef struct {
  psync_list list;
  char str[];
} string_list;

psync_malloc_t psync_malloc=malloc;
psync_realloc_t psync_realloc=realloc;
psync_free_t psync_free=free;

const char *psync_database=NULL;

static int psync_libstate=0;
static pthread_mutex_t psync_libstate_mutex=PTHREAD_MUTEX_INITIALIZER;

#define return_error(err) do {psync_error=err; return -1;} while (0)
#define return_isyncid(err) do {psync_error=err; return PSYNC_INVALID_SYNCID;} while (0)

uint32_t psync_get_last_error(){
  return psync_error;
}

void psync_set_database_path(const char *databasepath){
  psync_database=psync_strdup(databasepath);
}

void psync_set_alloc(psync_malloc_t malloc_call, psync_realloc_t realloc_call, psync_free_t free_call){
  psync_malloc=malloc_call;
  psync_realloc=realloc_call;
  psync_free=free_call;
}

int psync_init(){
  if (IS_DEBUG){
    pthread_mutex_lock(&psync_libstate_mutex);
    if (psync_libstate!=0){
      pthread_mutex_unlock(&psync_libstate_mutex);
      debug(D_BUG, "you are not supposed to call psync_init for a second time");
      return 0;
    }
  }
  psync_compat_init();
  if (!psync_database){
    psync_database=psync_get_default_database_path();
    if (unlikely_log(!psync_database)){
      if (IS_DEBUG)
        pthread_mutex_unlock(&psync_libstate_mutex);
      return_error(PERROR_NO_HOMEDIR);
    }
  }
  if (psync_sql_connect(psync_database) || psync_sql_statement(PSYNC_DATABASE_STRUCTURE)){
    if (IS_DEBUG)
      pthread_mutex_unlock(&psync_libstate_mutex);
    return_error(PERROR_DATABASE_OPEN);
  }
  if (unlikely_log(psync_ssl_init())){
    if (IS_DEBUG)
      pthread_mutex_unlock(&psync_libstate_mutex);
    return_error(PERROR_SSL_INIT_FAILED);
  }
  psync_libs_init();
  psync_settings_init();
  psync_status_init();
  if (IS_DEBUG){
    psync_libstate=1;
    pthread_mutex_unlock(&psync_libstate_mutex);
  }
  return 0;
}

void psync_start_sync(pstatus_change_callback_t status_callback, pevent_callback_t event_callback){
  if (IS_DEBUG){
    pthread_mutex_lock(&psync_libstate_mutex);
    if (psync_libstate==0){
      pthread_mutex_unlock(&psync_libstate_mutex);
      debug(D_BUG, "you are calling psync_start_sync before psync_init");
      return;
    }
    else if (psync_libstate==2){
      pthread_mutex_unlock(&psync_libstate_mutex);
      debug(D_BUG, "you are calling psync_start_sync for a second time");
      return;
    }
    else
      psync_libstate=2;
    pthread_mutex_unlock(&psync_libstate_mutex);
  }
  psync_timer_init();
  psync_diff_init();
  psync_upload_init();
  psync_download_init();
  psync_syncer_init();
  psync_netlibs_init();
  psync_localscan_init();
  psync_p2p_init();
  if (status_callback)
    psync_set_status_callback(status_callback);
  if (event_callback)
    psync_set_event_callback(event_callback);
}

uint32_t psync_download_state(){
  return 0;
}

void psync_destroy(){
  psync_do_run=0;
  psync_send_status_update();
  psync_timer_wake();
  psync_timer_notify_exception();
  psync_milisleep(20);
  psync_sql_lock();
  psync_sql_close();
}

void psync_get_status(pstatus_t *status){
  memcpy(status, &psync_status, sizeof(pstatus_t));
}

char *psync_get_username(){
  return psync_sql_cellstr("SELECT value FROM setting WHERE id='username'");
}

static void clear_db(int save){
  psync_sql_statement("DELETE FROM setting WHERE id IN ('pass', 'auth')");
  psync_setting_set_bool(_PS(saveauth), save);
}

void psync_set_user_pass(const char *username, const char *password, int save){
  clear_db(save);
  if (save){
    psync_set_string_value("user", username);
    psync_set_string_value("pass", password);
  }
  else{
    pthread_mutex_lock(&psync_my_auth_mutex);
    psync_free(psync_my_user);
    psync_my_user=psync_strdup(username);
    psync_free(psync_my_pass);
    psync_my_pass=psync_strdup(password);
    pthread_mutex_unlock(&psync_my_auth_mutex);
  }
  psync_set_status(PSTATUS_TYPE_AUTH, PSTATUS_AUTH_PROVIDED);
}

void psync_set_pass(const char *password, int save){
  clear_db(save);
  if (save)
    psync_set_string_value("pass", password);
  else{
    pthread_mutex_lock(&psync_my_auth_mutex);
    psync_free(psync_my_pass);
    psync_my_pass=psync_strdup(password);
    pthread_mutex_unlock(&psync_my_auth_mutex);
  }
  psync_set_status(PSTATUS_TYPE_AUTH, PSTATUS_AUTH_PROVIDED);
}

void psync_set_auth(const char *auth, int save){
  clear_db(save);
  if (save)
    psync_set_string_value("auth", auth);
  else
    strcpy(psync_my_auth, auth);
  psync_set_status(PSTATUS_TYPE_AUTH, PSTATUS_AUTH_PROVIDED);
}

void psync_unlink(){
  psync_sql_res *res;
  psync_variant_row row;
  char *sql;
  const char *str;
  size_t len;
  uint32_t runstatus;
  psync_list list;
  string_list *le;
  runstatus=psync_status_get(PSTATUS_TYPE_RUN);
  psync_set_status(PSTATUS_TYPE_RUN, PSTATUS_RUN_STOP);
  psync_timer_notify_exception();
  psync_milisleep(20);
  psync_sql_lock();
  psync_list_init(&list);
  res=psync_sql_query("SELECT name FROM sqlite_master WHERE type='index'");
  while ((row=psync_sql_fetch_row(res))){
    str=psync_get_lstring(row[0], &len);
    le=(string_list *)psync_malloc(offsetof(string_list, str)+len+1);
    memcpy(le->str, str, len+1);
    psync_list_add_tail(&list, &le->list);
  }
  psync_sql_free_result(res);
  psync_list_for_each_element(le, &list, string_list, list){
    sql=psync_strcat("DROP INDEX ", le->str, NULL);
    psync_sql_statement(sql);
    psync_free(sql);
  }
  psync_list_for_each_element_call(&list, string_list, list, psync_free);
  psync_list_init(&list);
  res=psync_sql_query("SELECT name FROM sqlite_master WHERE type='table'");
  while ((row=psync_sql_fetch_row(res))){
    str=psync_get_lstring(row[0], &len);
    le=(string_list *)psync_malloc(offsetof(string_list, str)+len+1);
    memcpy(le->str, str, len+1);
    psync_list_add_tail(&list, &le->list);
  }
  psync_sql_free_result(res);
  psync_list_for_each_element(le, &list, string_list, list){
    sql=psync_strcat("DROP TABLE ", le->str, NULL);
    psync_sql_statement(sql);
    psync_free(sql);
  }
  psync_list_for_each_element_call(&list, string_list, list, psync_free);
  psync_sql_statement("VACUUM");
  psync_sql_statement(PSYNC_DATABASE_STRUCTURE);
  pthread_mutex_lock(&psync_my_auth_mutex);
  memset(psync_my_auth, 0, sizeof(psync_my_auth));
  psync_my_user=NULL;
  psync_my_pass=NULL;
  psync_my_userid=0;
  pthread_mutex_unlock(&psync_my_auth_mutex);
  psync_sql_unlock();
  psync_set_status(PSTATUS_TYPE_AUTH, PSTATUS_AUTH_REQUIRED);
  psync_set_status(PSTATUS_TYPE_RUN, runstatus);
}

psync_syncid_t psync_add_sync_by_path(const char *localpath, const char *remotepath, psync_synctype_t synctype){
  psync_folderid_t folderid=psync_get_folderid_by_path(remotepath);
  if (likely_log(folderid!=PSYNC_INVALID_FOLDERID))
    return psync_add_sync_by_folderid(localpath, folderid, synctype);
  else
    return PSYNC_INVALID_SYNCID;
}

psync_syncid_t psync_add_sync_by_folderid(const char *localpath, psync_folderid_t folderid, psync_synctype_t synctype){
  psync_sql_res *res;
  psync_uint_row row;
  uint64_t perms;
  psync_stat_t st;
  psync_syncid_t ret;
  int unsigned md;
  if (unlikely_log(synctype<PSYNC_SYNCTYPE_MIN || synctype>PSYNC_SYNCTYPE_MAX))
    return_isyncid(PERROR_INVALID_SYNCTYPE);
  if (unlikely_log(psync_stat(localpath, &st)) || unlikely_log(!psync_stat_isfolder(&st)))
    return_isyncid(PERROR_LOCAL_FOLDER_NOT_FOUND);
  if (synctype&PSYNC_DOWNLOAD_ONLY)
    md=7;
  else
    md=5;
  if (unlikely_log(!psync_stat_mode_ok(&st, md)))
    return_isyncid(PERROR_LOCAL_FOLDER_ACC_DENIED);
  if (folderid){
    res=psync_sql_query("SELECT permissions FROM folder WHERE id=?");
    if (unlikely_log(!res))
      return_isyncid(PERROR_DATABASE_ERROR);
    psync_sql_bind_uint(res, 1, folderid);
    row=psync_sql_fetch_rowint(res);
    if (unlikely_log(!row)){
      psync_sql_free_result(res);
      return_isyncid(PERROR_REMOTE_FOLDER_NOT_FOUND);
    }
    perms=row[0];
    psync_sql_free_result(res);
  }
  else
    perms=PSYNC_PERM_ALL;
  if (unlikely_log((synctype&PSYNC_DOWNLOAD_ONLY && (perms&PSYNC_PERM_READ)!=PSYNC_PERM_READ) ||
      (synctype&PSYNC_UPLOAD_ONLY && (perms&PSYNC_PERM_WRITE)!=PSYNC_PERM_WRITE)))
    return_isyncid(PERROR_REMOTE_FOLDER_ACC_DENIED);
  res=psync_sql_prep_statement("INSERT OR IGNORE INTO syncfolder (folderid, localpath, synctype, flags) VALUES (?, ?, ?, 0)");
  if (unlikely_log(!res))
    return_isyncid(PERROR_DATABASE_ERROR);
  psync_sql_bind_uint(res, 1, folderid);
  psync_sql_bind_string(res, 2, localpath);
  psync_sql_bind_uint(res, 3, synctype);
  psync_sql_run(res);
  if (likely_log(psync_sql_affected_rows()))
    ret=psync_sql_insertid();
  else
    ret=PSYNC_INVALID_SYNCID;
  psync_sql_free_result(res);
  if (ret==PSYNC_INVALID_SYNCID)
    return_isyncid(PERROR_FOLDER_ALREADY_SYNCING);
  psync_syncer_new(ret);
  return ret;
}

int psync_add_sync_by_path_delayed(const char *localpath, const char *remotepath, psync_synctype_t synctype){
  psync_sql_res *res;
  psync_stat_t st;
  int unsigned md;
  if (unlikely_log(synctype<PSYNC_SYNCTYPE_MIN || synctype>PSYNC_SYNCTYPE_MAX))
    return_error(PERROR_INVALID_SYNCTYPE);
  if (unlikely_log(psync_stat(localpath, &st)) || unlikely_log(!psync_stat_isfolder(&st)))
    return_error(PERROR_LOCAL_FOLDER_NOT_FOUND);
  if (synctype&PSYNC_DOWNLOAD_ONLY)
    md=7;
  else
    md=5;
  if (unlikely_log(!psync_stat_mode_ok(&st, md)))
    return_error(PERROR_LOCAL_FOLDER_ACC_DENIED);
  res=psync_sql_prep_statement("INSERT INTO syncfolderdelayed (localpath, remotepath, synctype) VALUES (?, ?, ?)");
  psync_sql_bind_string(res, 1, localpath);
  psync_sql_bind_string(res, 2, remotepath);
  psync_sql_bind_uint(res, 3, synctype);
  psync_sql_run_free(res);
  return 0;
}

int psync_change_synctype(psync_syncid_t syncid, psync_synctype_t synctype);

int psync_delete_sync(psync_syncid_t syncid){
  psync_sql_res *res;
  psync_uint_row row;
  psync_sql_start_transaction();
  res=psync_sql_query("SELECT type, itemid, localitemid FROM task WHERE syncid=?");
  psync_sql_bind_uint(res, 1, syncid);
  while ((row=psync_sql_fetch_rowint(res)))
    if (row[0]==PSYNC_DOWNLOAD_FILE)
      psync_stop_file_download(row[1], syncid);
    else if (row[0]==PSYNC_UPLOAD_FILE)
      psync_delete_upload_tasks_for_file(row[2]);
  psync_sql_free_result(res);
  res=psync_sql_prep_statement("DELETE FROM syncfolder WHERE id=?");
  psync_sql_bind_uint(res, 1, syncid);
  psync_sql_run_free(res);
  if (psync_sql_commit_transaction())
    return -1;
  else{
    psync_stop_sync_download(syncid);
    return 0;
  }
}

psync_folder_list_t *psync_get_sync_list(){
  return psync_list_get_list();
}

psuggested_folders_t *psync_get_sync_suggestions(){
  char *home;
  psuggested_folders_t *ret;
  home=psync_get_home_dir();
  if (likely_log(home)){
    ret=psync_scanner_scan_folder(home);
    psync_free(home);
    return ret;
  }
  else{
    psync_error=PERROR_NO_HOMEDIR;
    return NULL;
  }
}

pfolder_list_t *psync_list_local_folder_by_path(const char *localpath, psync_listtype_t listtype){
  return psync_list_local_folder(localpath, listtype);
}

pfolder_list_t *psync_list_remote_folder_by_path(const char *remotepath, psync_listtype_t listtype){
  psync_folderid_t folderid=psync_get_folderid_by_path(remotepath);
  if (folderid!=PSYNC_INVALID_FOLDERID)
    return psync_list_remote_folder(folderid, listtype);
  else
    return NULL;
}

pfolder_list_t *psync_list_remote_folder_by_folderid(psync_folderid_t folderid, psync_listtype_t listtype){
  return psync_list_remote_folder(folderid, listtype);
}

int psync_is_name_to_ignore(const char *name){
  const char *ign, *sc, *pt;
  char *namelower;
  unsigned char *lp;
  size_t ilen, off, pl;
  namelower=psync_strdup(name);
  lp=(unsigned char *)namelower;
  while (*lp){
    *lp=tolower(*lp);
    lp++;
  }
  ign=psync_setting_get_string(_PS(ignorepatterns));
  ilen=strlen(ign);
  off=0;
  do {
    sc=(const char *)memchr(ign+off, ';', ilen-off);
    if (sc)
      pl=sc-ign-off;
    else
      pl=ilen-off;
    pt=ign+off;
    off+=pl+1;
    while (pl && isspace((unsigned char)*pt)){
      pt++;
      pl--;
    }
    while (pl && isspace((unsigned char)pt[pl-1]))
      pl--;
    if (psync_match_pattern(namelower, pt, pl)){
      psync_free(namelower);
      debug(D_NOTICE, "ignoring file/folder %s", name);
      return 1;
    }
  } while (sc);
  psync_free(namelower);
  return 0;
}

static void psync_set_run_status(uint32_t status){
  psync_set_status(PSTATUS_TYPE_RUN, status);
  psync_set_uint_value("runstatus", status);
}

int psync_pause(){
  psync_set_run_status(PSTATUS_RUN_PAUSE);
  return 0;
}

int psync_stop(){
  psync_set_run_status(PSTATUS_RUN_STOP);
  psync_timer_notify_exception();
  return 0;
}

int psync_resume(){
  psync_set_run_status(PSTATUS_RUN_RUN);
  return 0;
}

void psync_run_localscan(){
  psync_wake_localscan();
}

int psync_register(const char *email, const char *password, int termsaccepted, char **err){
  psync_socket *api;
  binresult *res;
  uint64_t ret;
  binparam params[]={P_STR("mail", email), P_STR("password", password), P_STR("termsaccepted", termsaccepted?"yes":"0"), P_NUM("os", P_OS_ID)};
  api=psync_apipool_get();
  if (unlikely_log(!api))
    goto neterr1;
  res=send_command(api, "register", params);
  psync_apipool_release(api);
  if (unlikely_log(!res))
    goto neterr1;
  ret=psync_find_result(res, "result", PARAM_NUM)->num;
  if (ret){
    debug(D_WARNING, "register method returned error %lu", (long unsigned)ret);
    if (err)
      *err=psync_strdup(psync_find_result(res, "error", PARAM_STR)->str);
  }
  psync_free(res);
  return ret;
neterr1:
  if (err)
    *err=psync_strdup("Could not connect to the server.");
  return -1;
}

int psync_get_bool_setting(const char *settingname){
  return psync_setting_get_bool(psync_setting_getid(settingname));
}

int psync_set_bool_setting(const char *settingname, int value){
  return psync_setting_set_bool(psync_setting_getid(settingname), value);
}

int64_t psync_get_int_setting(const char *settingname){
  return psync_setting_get_int(psync_setting_getid(settingname));
}

int psync_set_int_setting(const char *settingname, int64_t value){
  return psync_setting_set_int(psync_setting_getid(settingname), value);
}

uint64_t psync_get_uint_setting(const char *settingname){
  return psync_setting_get_uint(psync_setting_getid(settingname));
}

int psync_set_uint_setting(const char *settingname, uint64_t value){
  return psync_setting_set_uint(psync_setting_getid(settingname), value);
}

const char *psync_get_string_setting(const char *settingname){
  return psync_setting_get_string(psync_setting_getid(settingname));
}

int psync_set_string_setting(const char *settingname, const char *value){
  return psync_setting_set_string(psync_setting_getid(settingname), value);
}

int psync_has_value(const char *valuename){
  psync_sql_res *res;
  psync_uint_row row;
  int ret;
  res=psync_sql_query("SELECT COUNT(*) FROM setting WHERE id=?");
  psync_sql_bind_string(res, 1, valuename);
  row=psync_sql_fetch_rowint(res);
  if (row)
    ret=row[0];
  else
    ret=0;
  psync_sql_free_result(res);
  return ret;
}

int psync_get_bool_value(const char *valuename){
  return !!psync_get_uint_value(valuename);
}

void psync_set_bool_value(const char *valuename, int value){
  psync_set_uint_value(valuename, (uint64_t)(!!value));
}

int64_t psync_get_int_value(const char *valuename){
  return (int64_t)psync_get_uint_value(valuename);
}

void psync_set_int_value(const char *valuename, int64_t value){
  psync_set_uint_value(valuename, (uint64_t)value);
}

uint64_t psync_get_uint_value(const char *valuename){
  psync_sql_res *res;
  psync_uint_row row;
  uint64_t ret;
  res=psync_sql_query("SELECT value FROM setting WHERE id=?");
  psync_sql_bind_string(res, 1, valuename);
  row=psync_sql_fetch_rowint(res);
  if (row)
    ret=row[0];
  else
    ret=0;
  psync_sql_free_result(res);
  return ret;
}

void psync_set_uint_value(const char *valuename, uint64_t value){
  psync_sql_res *res;
  res=psync_sql_prep_statement("REPLACE INTO setting (id, value) VALUES (?, ?)");
  psync_sql_bind_string(res, 1, valuename);
  psync_sql_bind_uint(res, 2, value);
  psync_sql_run_free(res);
}

char *psync_get_string_value(const char *valuename){
  psync_sql_res *res;
  psync_str_row row;
  char *ret;
  res=psync_sql_query("SELECT value FROM setting WHERE id=?");
  psync_sql_bind_string(res, 1, valuename);
  row=psync_sql_fetch_rowstr(res);
  if (row)
    ret=psync_strdup(row[0]);
  else
    ret=NULL;
  psync_sql_free_result(res);
  return ret;
}

void psync_set_string_value(const char *valuename, const char *value){
  psync_sql_res *res;
  res=psync_sql_prep_statement("REPLACE INTO setting (id, value) VALUES (?, ?)");
  psync_sql_bind_string(res, 1, valuename);
  psync_sql_bind_string(res, 2, value);
  psync_sql_run_free(res);
}

