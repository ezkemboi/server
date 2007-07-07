/* Copyright (C) 2000-2004 MySQL AB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */


/* Function with list databases, tables or fields */

#include "mysql_priv.h"
#include "sql_select.h"                         // For select_describe
#include "sql_show.h"
#include "repl_failsafe.h"
#include "sp.h"
#include "sp_head.h"
#include "sql_trigger.h"
#include "authors.h"
#include "contributors.h"
#include "events.h"
#include "event_data_objects.h"
#include <my_dir.h>

#ifdef WITH_PARTITION_STORAGE_ENGINE
#include "ha_partition.h"
#endif
enum enum_i_s_events_fields
{
  ISE_EVENT_CATALOG= 0,
  ISE_EVENT_SCHEMA,
  ISE_EVENT_NAME,
  ISE_DEFINER,
  ISE_TIME_ZONE,
  ISE_EVENT_BODY,
  ISE_EVENT_DEFINITION,
  ISE_EVENT_TYPE,
  ISE_EXECUTE_AT,
  ISE_INTERVAL_VALUE,
  ISE_INTERVAL_FIELD,
  ISE_SQL_MODE,
  ISE_STARTS,
  ISE_ENDS,
  ISE_STATUS,
  ISE_ON_COMPLETION,
  ISE_CREATED,
  ISE_LAST_ALTERED,
  ISE_LAST_EXECUTED,
  ISE_EVENT_COMMENT,
  ISE_ORIGINATOR,
  ISE_CLIENT_CS,
  ISE_CONNECTION_CL,
  ISE_DB_CL
};

#ifndef NO_EMBEDDED_ACCESS_CHECKS
static const char *grant_names[]={
  "select","insert","update","delete","create","drop","reload","shutdown",
  "process","file","grant","references","index","alter"};

static TYPELIB grant_types = { sizeof(grant_names)/sizeof(char **),
                               "grant_types",
                               grant_names, NULL};
#endif

static void store_key_options(THD *thd, String *packet, TABLE *table,
                              KEY *key_info);

static void
append_algorithm(TABLE_LIST *table, String *buff);


/***************************************************************************
** List all table types supported
***************************************************************************/

static int make_version_string(char *buf, int buf_length, uint version)
{
  return my_snprintf(buf, buf_length, "%d.%d", version>>8,version&0xff);
}

static my_bool show_plugins(THD *thd, plugin_ref plugin,
                            void *arg)
{
  TABLE *table= (TABLE*) arg;
  struct st_mysql_plugin *plug= plugin_decl(plugin);
  struct st_plugin_dl *plugin_dl= plugin_dlib(plugin);
  CHARSET_INFO *cs= system_charset_info;
  char version_buf[20];

  restore_record(table, s->default_values);

  table->field[0]->store(plugin_name(plugin)->str,
                         plugin_name(plugin)->length, cs);

  table->field[1]->store(version_buf,
        make_version_string(version_buf, sizeof(version_buf), plug->version),
        cs);


  switch (plugin_state(plugin)) {
  /* case PLUGIN_IS_FREED: does not happen */
  case PLUGIN_IS_DELETED:
    table->field[2]->store(STRING_WITH_LEN("DELETED"), cs);
    break;
  case PLUGIN_IS_UNINITIALIZED:
    table->field[2]->store(STRING_WITH_LEN("INACTIVE"), cs);
    break;
  case PLUGIN_IS_READY:
    table->field[2]->store(STRING_WITH_LEN("ACTIVE"), cs);
    break;
  default:
    DBUG_ASSERT(0);
  }

  table->field[3]->store(plugin_type_names[plug->type].str,
                         plugin_type_names[plug->type].length,
                         cs);
  table->field[4]->store(version_buf,
        make_version_string(version_buf, sizeof(version_buf),
                            *(uint *)plug->info), cs);

  if (plugin_dl)
  {
    table->field[5]->store(plugin_dl->dl.str, plugin_dl->dl.length, cs);
    table->field[5]->set_notnull();
    table->field[6]->store(version_buf,
          make_version_string(version_buf, sizeof(version_buf),
                              plugin_dl->version),
          cs);
    table->field[6]->set_notnull();
  }
  else
  {
    table->field[5]->set_null();
    table->field[6]->set_null();
  }


  if (plug->author)
  {
    table->field[7]->store(plug->author, strlen(plug->author), cs);
    table->field[7]->set_notnull();
  }
  else
    table->field[7]->set_null();

  if (plug->descr)
  {
    table->field[8]->store(plug->descr, strlen(plug->descr), cs);
    table->field[8]->set_notnull();
  }
  else
    table->field[8]->set_null();

  switch (plug->license) {
  case PLUGIN_LICENSE_GPL:
    table->field[9]->store(PLUGIN_LICENSE_GPL_STRING, 
                           strlen(PLUGIN_LICENSE_GPL_STRING), cs);
    break;
  case PLUGIN_LICENSE_BSD:
    table->field[9]->store(PLUGIN_LICENSE_BSD_STRING, 
                           strlen(PLUGIN_LICENSE_BSD_STRING), cs);
    break;
  default:
    table->field[9]->store(PLUGIN_LICENSE_PROPRIETARY_STRING, 
                           strlen(PLUGIN_LICENSE_PROPRIETARY_STRING), cs);
    break;
  }
  table->field[9]->set_notnull();

  return schema_table_store_record(thd, table);
}


int fill_plugins(THD *thd, TABLE_LIST *tables, COND *cond)
{
  DBUG_ENTER("fill_plugins");
  TABLE *table= tables->table;

  if (plugin_foreach_with_mask(thd, show_plugins, MYSQL_ANY_PLUGIN,
                               ~PLUGIN_IS_FREED, table))
    DBUG_RETURN(1);

  DBUG_RETURN(0);
}


/***************************************************************************
** List all Authors.
** If you can update it, you get to be in it :)
***************************************************************************/

bool mysqld_show_authors(THD *thd)
{
  List<Item> field_list;
  Protocol *protocol= thd->protocol;
  DBUG_ENTER("mysqld_show_authors");

  field_list.push_back(new Item_empty_string("Name",40));
  field_list.push_back(new Item_empty_string("Location",40));
  field_list.push_back(new Item_empty_string("Comment",80));

  if (protocol->send_fields(&field_list,
                            Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF))
    DBUG_RETURN(TRUE);

  show_table_authors_st *authors;
  for (authors= show_table_authors; authors->name; authors++)
  {
    protocol->prepare_for_resend();
    protocol->store(authors->name, system_charset_info);
    protocol->store(authors->location, system_charset_info);
    protocol->store(authors->comment, system_charset_info);
    if (protocol->write())
      DBUG_RETURN(TRUE);
  }
  send_eof(thd);
  DBUG_RETURN(FALSE);
}


/***************************************************************************
** List all Contributors.
** Please get permission before updating
***************************************************************************/

bool mysqld_show_contributors(THD *thd)
{
  List<Item> field_list;
  Protocol *protocol= thd->protocol;
  DBUG_ENTER("mysqld_show_contributors");

  field_list.push_back(new Item_empty_string("Name",40));
  field_list.push_back(new Item_empty_string("Location",40));
  field_list.push_back(new Item_empty_string("Comment",80));

  if (protocol->send_fields(&field_list,
                            Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF))
    DBUG_RETURN(TRUE);

  show_table_contributors_st *contributors;
  for (contributors= show_table_contributors; contributors->name; contributors++)
  {
    protocol->prepare_for_resend();
    protocol->store(contributors->name, system_charset_info);
    protocol->store(contributors->location, system_charset_info);
    protocol->store(contributors->comment, system_charset_info);
    if (protocol->write())
      DBUG_RETURN(TRUE);
  }
  send_eof(thd);
  DBUG_RETURN(FALSE);
}


/***************************************************************************
 List all privileges supported
***************************************************************************/

struct show_privileges_st {
  const char *privilege;
  const char *context;
  const char *comment;
};

static struct show_privileges_st sys_privileges[]=
{
  {"Alter", "Tables",  "To alter the table"},
  {"Alter routine", "Functions,Procedures",  "To alter or drop stored functions/procedures"},
  {"Create", "Databases,Tables,Indexes",  "To create new databases and tables"},
  {"Create routine","Functions,Procedures","To use CREATE FUNCTION/PROCEDURE"},
  {"Create temporary tables","Databases","To use CREATE TEMPORARY TABLE"},
  {"Create view", "Tables",  "To create new views"},
  {"Create user", "Server Admin",  "To create new users"},
  {"Delete", "Tables",  "To delete existing rows"},
  {"Drop", "Databases,Tables", "To drop databases, tables, and views"},
  {"Event","Server Admin","To create, alter, drop and execute events"},
  {"Execute", "Functions,Procedures", "To execute stored routines"},
  {"File", "File access on server",   "To read and write files on the server"},
  {"Grant option",  "Databases,Tables,Functions,Procedures", "To give to other users those privileges you possess"},
  {"Index", "Tables",  "To create or drop indexes"},
  {"Insert", "Tables",  "To insert data into tables"},
  {"Lock tables","Databases","To use LOCK TABLES (together with SELECT privilege)"},
  {"Process", "Server Admin", "To view the plain text of currently executing queries"},
  {"References", "Databases,Tables", "To have references on tables"},
  {"Reload", "Server Admin", "To reload or refresh tables, logs and privileges"},
  {"Replication client","Server Admin","To ask where the slave or master servers are"},
  {"Replication slave","Server Admin","To read binary log events from the master"},
  {"Select", "Tables",  "To retrieve rows from table"},
  {"Show databases","Server Admin","To see all databases with SHOW DATABASES"},
  {"Show view","Tables","To see views with SHOW CREATE VIEW"},
  {"Shutdown","Server Admin", "To shut down the server"},
  {"Super","Server Admin","To use KILL thread, SET GLOBAL, CHANGE MASTER, etc."},
  {"Trigger","Tables", "To use triggers"},
  {"Update", "Tables",  "To update existing rows"},
  {"Usage","Server Admin","No privileges - allow connect only"},
  {NullS, NullS, NullS}
};

bool mysqld_show_privileges(THD *thd)
{
  List<Item> field_list;
  Protocol *protocol= thd->protocol;
  DBUG_ENTER("mysqld_show_privileges");

  field_list.push_back(new Item_empty_string("Privilege",10));
  field_list.push_back(new Item_empty_string("Context",15));
  field_list.push_back(new Item_empty_string("Comment",NAME_CHAR_LEN));

  if (protocol->send_fields(&field_list,
                            Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF))
    DBUG_RETURN(TRUE);

  show_privileges_st *privilege= sys_privileges;
  for (privilege= sys_privileges; privilege->privilege ; privilege++)
  {
    protocol->prepare_for_resend();
    protocol->store(privilege->privilege, system_charset_info);
    protocol->store(privilege->context, system_charset_info);
    protocol->store(privilege->comment, system_charset_info);
    if (protocol->write())
      DBUG_RETURN(TRUE);
  }
  send_eof(thd);
  DBUG_RETURN(FALSE);
}


/***************************************************************************
  List all column types
***************************************************************************/

struct show_column_type_st
{
  const char *type;
  uint size;
  const char *min_value;
  const char *max_value;
  uint precision;
  uint scale;
  const char *nullable;
  const char *auto_increment;
  const char *unsigned_attr;
  const char *zerofill;
  const char *searchable;
  const char *case_sensitivity;
  const char *default_value;
  const char *comment;
};

/* TODO: Add remaning types */

static struct show_column_type_st sys_column_types[]=
{
  {"tinyint",
    1,  "-128",  "127",  0,  0,  "YES",  "YES",
    "NO",   "YES", "YES",  "NO",  "NULL,0",
    "A very small integer"},
  {"tinyint unsigned",
    1,  "0"   ,  "255",  0,  0,  "YES",  "YES",
    "YES",  "YES",  "YES",  "NO",  "NULL,0",
    "A very small integer"},
};

bool mysqld_show_column_types(THD *thd)
{
  List<Item> field_list;
  Protocol *protocol= thd->protocol;
  DBUG_ENTER("mysqld_show_column_types");

  field_list.push_back(new Item_empty_string("Type",30));
  field_list.push_back(new Item_int("Size",(longlong) 1,
                                    MY_INT64_NUM_DECIMAL_DIGITS));
  field_list.push_back(new Item_empty_string("Min_Value",20));
  field_list.push_back(new Item_empty_string("Max_Value",20));
  field_list.push_back(new Item_return_int("Prec", 4, MYSQL_TYPE_SHORT));
  field_list.push_back(new Item_return_int("Scale", 4, MYSQL_TYPE_SHORT));
  field_list.push_back(new Item_empty_string("Nullable",4));
  field_list.push_back(new Item_empty_string("Auto_Increment",4));
  field_list.push_back(new Item_empty_string("Unsigned",4));
  field_list.push_back(new Item_empty_string("Zerofill",4));
  field_list.push_back(new Item_empty_string("Searchable",4));
  field_list.push_back(new Item_empty_string("Case_Sensitive",4));
  field_list.push_back(new Item_empty_string("Default",NAME_CHAR_LEN));
  field_list.push_back(new Item_empty_string("Comment",NAME_CHAR_LEN));

  if (protocol->send_fields(&field_list,
                            Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF))
    DBUG_RETURN(TRUE);

  /* TODO: Change the loop to not use 'i' */
  for (uint i=0; i < sizeof(sys_column_types)/sizeof(sys_column_types[0]); i++)
  {
    protocol->prepare_for_resend();
    protocol->store(sys_column_types[i].type, system_charset_info);
    protocol->store((ulonglong) sys_column_types[i].size);
    protocol->store(sys_column_types[i].min_value, system_charset_info);
    protocol->store(sys_column_types[i].max_value, system_charset_info);
    protocol->store_short((longlong) sys_column_types[i].precision);
    protocol->store_short((longlong) sys_column_types[i].scale);
    protocol->store(sys_column_types[i].nullable, system_charset_info);
    protocol->store(sys_column_types[i].auto_increment, system_charset_info);
    protocol->store(sys_column_types[i].unsigned_attr, system_charset_info);
    protocol->store(sys_column_types[i].zerofill, system_charset_info);
    protocol->store(sys_column_types[i].searchable, system_charset_info);
    protocol->store(sys_column_types[i].case_sensitivity, system_charset_info);
    protocol->store(sys_column_types[i].default_value, system_charset_info);
    protocol->store(sys_column_types[i].comment, system_charset_info);
    if (protocol->write())
      DBUG_RETURN(TRUE);
  }
  send_eof(thd);
  DBUG_RETURN(FALSE);
}


/*
  find_files() - find files in a given directory.

  SYNOPSIS
    find_files()
    thd                 thread handler
    files               put found files in this list
    db                  database name to set in TABLE_LIST structure
    path                path to database
    wild                filter for found files
    dir                 read databases in path if TRUE, read .frm files in
                        database otherwise

  RETURN
    FIND_FILES_OK       success
    FIND_FILES_OOM      out of memory error
    FIND_FILES_DIR      no such directory, or directory can't be read
*/


find_files_result
find_files(THD *thd, List<char> *files, const char *db,
           const char *path, const char *wild, bool dir)
{
  uint i;
  char *ext;
  MY_DIR *dirp;
  FILEINFO *file;
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  uint col_access=thd->col_access;
#endif
  TABLE_LIST table_list;
  DBUG_ENTER("find_files");

  if (wild && !wild[0])
    wild=0;

  bzero((char*) &table_list,sizeof(table_list));

  if (!(dirp = my_dir(path,MYF(dir ? MY_WANT_STAT : 0))))
  {
    if (my_errno == ENOENT)
      my_error(ER_BAD_DB_ERROR, MYF(ME_BELL+ME_WAITTANG), db);
    else
      my_error(ER_CANT_READ_DIR, MYF(ME_BELL+ME_WAITTANG), path, my_errno);
    DBUG_RETURN(FIND_FILES_DIR);
  }

  for (i=0 ; i < (uint) dirp->number_off_files  ; i++)
  {
    char uname[NAME_LEN + 1];                   /* Unencoded name */
    file=dirp->dir_entry+i;
    if (dir)
    {                                           /* Return databases */
      if ((file->name[0] == '.' && 
          ((file->name[1] == '.' && file->name[2] == '\0') ||
            file->name[1] == '\0')))
        continue;                               /* . or .. */
#ifdef USE_SYMDIR
      char *ext;
      char buff[FN_REFLEN];
      if (my_use_symdir && !strcmp(ext=fn_ext(file->name), ".sym"))
      {
	/* Only show the sym file if it points to a directory */
	char *end;
        *ext=0;                                 /* Remove extension */
	unpack_dirname(buff, file->name);
	end= strend(buff);
	if (end != buff && end[-1] == FN_LIBCHAR)
	  end[-1]= 0;				// Remove end FN_LIBCHAR
        if (!my_stat(buff, file->mystat, MYF(0)))
               continue;
       }
#endif
      if (!MY_S_ISDIR(file->mystat->st_mode))
        continue;
      VOID(filename_to_tablename(file->name, uname, sizeof(uname)));
      if (wild && wild_compare(uname, wild, 0))
        continue;
      file->name= uname;
    }
    else
    {
        // Return only .frm files which aren't temp files.
      if (my_strcasecmp(system_charset_info, ext=fn_rext(file->name),reg_ext) ||
          is_prefix(file->name, tmp_file_prefix))
        continue;
      *ext=0;
      VOID(filename_to_tablename(file->name, uname, sizeof(uname)));
      file->name= uname;
      if (wild)
      {
	if (lower_case_table_names)
	{
	  if (wild_case_compare(files_charset_info, file->name, wild))
	    continue;
	}
	else if (wild_compare(file->name,wild,0))
	  continue;
      }
    }
#ifndef NO_EMBEDDED_ACCESS_CHECKS
    /* Don't show tables where we don't have any privileges */
    if (db && !(col_access & TABLE_ACLS))
    {
      table_list.db= (char*) db;
      table_list.db_length= strlen(db);
      table_list.table_name= file->name;
      table_list.table_name_length= strlen(file->name);
      table_list.grant.privilege=col_access;
      if (check_grant(thd, TABLE_ACLS, &table_list, 1, 1, 1))
        continue;
    }
#endif
    if (files->push_back(thd->strdup(file->name)))
    {
      my_dirend(dirp);
      DBUG_RETURN(FIND_FILES_OOM);
    }
  }
  DBUG_PRINT("info",("found: %d files", files->elements));
  my_dirend(dirp);

  VOID(ha_find_files(thd,db,path,wild,dir,files));

  DBUG_RETURN(FIND_FILES_OK);
}


bool
mysqld_show_create(THD *thd, TABLE_LIST *table_list)
{
  Protocol *protocol= thd->protocol;
  char buff[2048];
  String buffer(buff, sizeof(buff), system_charset_info);
  DBUG_ENTER("mysqld_show_create");
  DBUG_PRINT("enter",("db: %s  table: %s",table_list->db,
                      table_list->table_name));

  /* We want to preserve the tree for views. */
  thd->lex->view_prepare_mode= TRUE;

  /* Only one table for now, but VIEW can involve several tables */
  if (open_normal_and_derived_tables(thd, table_list, 0))
  {
    if (!table_list->view || thd->net.last_errno != ER_VIEW_INVALID)
      DBUG_RETURN(TRUE);

    /*
      Clear all messages with 'error' level status and
      issue a warning with 'warning' level status in 
      case of invalid view and last error is ER_VIEW_INVALID
    */
    mysql_reset_errors(thd, true);
    thd->clear_error();

    push_warning_printf(thd,MYSQL_ERROR::WARN_LEVEL_WARN,
                        ER_VIEW_INVALID,
                        ER(ER_VIEW_INVALID),
                        table_list->view_db.str,
                        table_list->view_name.str);
  }

  /* TODO: add environment variables show when it become possible */
  if (thd->lex->only_view && !table_list->view)
  {
    my_error(ER_WRONG_OBJECT, MYF(0),
             table_list->db, table_list->table_name, "VIEW");
    DBUG_RETURN(TRUE);
  }

  buffer.length(0);

  if (table_list->view)
    buffer.set_charset(table_list->view_creation_ctx->get_client_cs());

  if ((table_list->view ?
       view_store_create_info(thd, table_list, &buffer) :
       store_create_info(thd, table_list, &buffer, NULL)))
    DBUG_RETURN(TRUE);

  List<Item> field_list;
  if (table_list->view)
  {
    field_list.push_back(new Item_empty_string("View",NAME_CHAR_LEN));
    field_list.push_back(new Item_empty_string("Create View",
                                               max(buffer.length(),1024)));
    field_list.push_back(new Item_empty_string("character_set_client",
                                               MY_CS_NAME_SIZE));
    field_list.push_back(new Item_empty_string("collation_connection",
                                               MY_CS_NAME_SIZE));
  }
  else
  {
    field_list.push_back(new Item_empty_string("Table",NAME_CHAR_LEN));
    // 1024 is for not to confuse old clients
    field_list.push_back(new Item_empty_string("Create Table",
                                               max(buffer.length(),1024)));
  }

  if (protocol->send_fields(&field_list,
                            Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF))
    DBUG_RETURN(TRUE);
  protocol->prepare_for_resend();
  if (table_list->view)
    protocol->store(table_list->view_name.str, system_charset_info);
  else
  {
    if (table_list->schema_table)
      protocol->store(table_list->schema_table->table_name,
                      system_charset_info);
    else
      protocol->store(table_list->table->alias, system_charset_info);
  }

  if (table_list->view)
  {
    protocol->store(buffer.ptr(), buffer.length(), &my_charset_bin);

    protocol->store(table_list->view_creation_ctx->get_client_cs()->csname,
                    system_charset_info);

    protocol->store(table_list->view_creation_ctx->get_connection_cl()->name,
                    system_charset_info);
  }
  else
    protocol->store(buffer.ptr(), buffer.length(), buffer.charset());

  if (protocol->write())
    DBUG_RETURN(TRUE);

  send_eof(thd);
  DBUG_RETURN(FALSE);
}

bool mysqld_show_create_db(THD *thd, char *dbname,
                           HA_CREATE_INFO *create_info)
{
  char buff[2048];
  String buffer(buff, sizeof(buff), system_charset_info);
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  Security_context *sctx= thd->security_ctx;
  uint db_access;
#endif
  HA_CREATE_INFO create;
  uint create_options = create_info ? create_info->options : 0;
  Protocol *protocol=thd->protocol;
  DBUG_ENTER("mysql_show_create_db");

#ifndef NO_EMBEDDED_ACCESS_CHECKS
  if (test_all_bits(sctx->master_access, DB_ACLS))
    db_access=DB_ACLS;
  else
    db_access= (acl_get(sctx->host, sctx->ip, sctx->priv_user, dbname, 0) |
		sctx->master_access);
  if (!(db_access & DB_ACLS) && check_grant_db(thd,dbname))
  {
    my_error(ER_DBACCESS_DENIED_ERROR, MYF(0),
             sctx->priv_user, sctx->host_or_ip, dbname);
    general_log_print(thd,COM_INIT_DB,ER(ER_DBACCESS_DENIED_ERROR),
                      sctx->priv_user, sctx->host_or_ip, dbname);
    DBUG_RETURN(TRUE);
  }
#endif
  if (!my_strcasecmp(system_charset_info, dbname,
                     INFORMATION_SCHEMA_NAME.str))
  {
    dbname= INFORMATION_SCHEMA_NAME.str;
    create.default_table_charset= system_charset_info;
  }
  else
  {
    if (check_db_dir_existence(dbname))
    {
      my_error(ER_BAD_DB_ERROR, MYF(0), dbname);
      DBUG_RETURN(TRUE);
    }

    load_db_opt_by_name(thd, dbname, &create);
  }
  List<Item> field_list;
  field_list.push_back(new Item_empty_string("Database",NAME_CHAR_LEN));
  field_list.push_back(new Item_empty_string("Create Database",1024));

  if (protocol->send_fields(&field_list,
                            Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF))
    DBUG_RETURN(TRUE);

  protocol->prepare_for_resend();
  protocol->store(dbname, strlen(dbname), system_charset_info);
  buffer.length(0);
  buffer.append(STRING_WITH_LEN("CREATE DATABASE "));
  if (create_options & HA_LEX_CREATE_IF_NOT_EXISTS)
    buffer.append(STRING_WITH_LEN("/*!32312 IF NOT EXISTS*/ "));
  append_identifier(thd, &buffer, dbname, strlen(dbname));

  if (create.default_table_charset)
  {
    buffer.append(STRING_WITH_LEN(" /*!40100"));
    buffer.append(STRING_WITH_LEN(" DEFAULT CHARACTER SET "));
    buffer.append(create.default_table_charset->csname);
    if (!(create.default_table_charset->state & MY_CS_PRIMARY))
    {
      buffer.append(STRING_WITH_LEN(" COLLATE "));
      buffer.append(create.default_table_charset->name);
    }
    buffer.append(STRING_WITH_LEN(" */"));
  }
  protocol->store(buffer.ptr(), buffer.length(), buffer.charset());

  if (protocol->write())
    DBUG_RETURN(TRUE);
  send_eof(thd);
  DBUG_RETURN(FALSE);
}



/****************************************************************************
  Return only fields for API mysql_list_fields
  Use "show table wildcard" in mysql instead of this
****************************************************************************/

void
mysqld_list_fields(THD *thd, TABLE_LIST *table_list, const char *wild)
{
  TABLE *table;
  DBUG_ENTER("mysqld_list_fields");
  DBUG_PRINT("enter",("table: %s",table_list->table_name));

  if (open_normal_and_derived_tables(thd, table_list, 0))
    DBUG_VOID_RETURN;
  table= table_list->table;

  List<Item> field_list;

  Field **ptr,*field;
  for (ptr=table->field ; (field= *ptr); ptr++)
  {
    if (!wild || !wild[0] || 
        !wild_case_compare(system_charset_info, field->field_name,wild))
    {
      if (table_list->view)
        field_list.push_back(new Item_ident_for_show(field,
                                                     table_list->view_db.str,
                                                     table_list->view_name.str));
      else
        field_list.push_back(new Item_field(field));
    }
  }
  restore_record(table, s->default_values);              // Get empty record
  table->use_all_columns();
  if (thd->protocol->send_fields(&field_list, Protocol::SEND_DEFAULTS |
                                              Protocol::SEND_EOF))
    DBUG_VOID_RETURN;
  thd->protocol->flush();
  DBUG_VOID_RETURN;
}


int
mysqld_dump_create_info(THD *thd, TABLE_LIST *table_list, int fd)
{
  Protocol *protocol= thd->protocol;
  String *packet= protocol->storage_packet();
  DBUG_ENTER("mysqld_dump_create_info");
  DBUG_PRINT("enter",("table: %s",table_list->table->s->table_name.str));

  protocol->prepare_for_resend();
  if (store_create_info(thd, table_list, packet, NULL))
    DBUG_RETURN(-1);

  if (fd < 0)
  {
    if (protocol->write())
      DBUG_RETURN(-1);
    protocol->flush();
  }
  else
  {
    if (my_write(fd, (const uchar*) packet->ptr(), packet->length(),
		 MYF(MY_WME)))
      DBUG_RETURN(-1);
  }
  DBUG_RETURN(0);
}

/*
  Go through all character combinations and ensure that sql_lex.cc can
  parse it as an identifier.

  SYNOPSIS
  require_quotes()
  name			attribute name
  name_length		length of name

  RETURN
    #	Pointer to conflicting character
    0	No conflicting character
*/

static const char *require_quotes(const char *name, uint name_length)
{
  uint length;
  bool pure_digit= TRUE;
  const char *end= name + name_length;

  for (; name < end ; name++)
  {
    uchar chr= (uchar) *name;
    length= my_mbcharlen(system_charset_info, chr);
    if (length == 1 && !system_charset_info->ident_map[chr])
      return name;
    if (length == 1 && (chr < '0' || chr > '9'))
      pure_digit= FALSE;
  }
  if (pure_digit)
    return name;
  return 0;
}


/*
  Quote the given identifier if needed and append it to the target string.
  If the given identifier is empty, it will be quoted.

  SYNOPSIS
  append_identifier()
  thd                   thread handler
  packet                target string
  name                  the identifier to be appended
  name_length           length of the appending identifier
*/

void
append_identifier(THD *thd, String *packet, const char *name, uint length)
{
  const char *name_end;
  char quote_char;
  int q= get_quote_char_for_identifier(thd, name, length);

  if (q == EOF)
  {
    packet->append(name, length, packet->charset());
    return;
  }

  /*
    The identifier must be quoted as it includes a quote character or
   it's a keyword
  */

  VOID(packet->reserve(length*2 + 2));
  quote_char= (char) q;
  packet->append(&quote_char, 1, system_charset_info);

  for (name_end= name+length ; name < name_end ; name+= length)
  {
    uchar chr= (uchar) *name;
    length= my_mbcharlen(system_charset_info, chr);
    /*
      my_mbcharlen can return 0 on a wrong multibyte
      sequence. It is possible when upgrading from 4.0,
      and identifier contains some accented characters.
      The manual says it does not work. So we'll just
      change length to 1 not to hang in the endless loop.
    */
    if (!length)
      length= 1;
    if (length == 1 && chr == (uchar) quote_char)
      packet->append(&quote_char, 1, system_charset_info);
    packet->append(name, length, system_charset_info);
  }
  packet->append(&quote_char, 1, system_charset_info);
}


/*
  Get the quote character for displaying an identifier.

  SYNOPSIS
    get_quote_char_for_identifier()
    thd		Thread handler
    name	name to quote
    length	length of name

  IMPLEMENTATION
    Force quoting in the following cases:
      - name is empty (for one, it is possible when we use this function for
        quoting user and host names for DEFINER clause);
      - name is a keyword;
      - name includes a special character;
    Otherwise identifier is quoted only if the option OPTION_QUOTE_SHOW_CREATE
    is set.

  RETURN
    EOF	  No quote character is needed
    #	  Quote character
*/

int get_quote_char_for_identifier(THD *thd, const char *name, uint length)
{
  if (length &&
      !is_keyword(name,length) &&
      !require_quotes(name, length) &&
      !(thd->options & OPTION_QUOTE_SHOW_CREATE))
    return EOF;
  if (thd->variables.sql_mode & MODE_ANSI_QUOTES)
    return '"';
  return '`';
}


/* Append directory name (if exists) to CREATE INFO */

static void append_directory(THD *thd, String *packet, const char *dir_type,
			     const char *filename)
{
  if (filename && !(thd->variables.sql_mode & MODE_NO_DIR_IN_CREATE))
  {
    uint length= dirname_length(filename);
    packet->append(' ');
    packet->append(dir_type);
    packet->append(STRING_WITH_LEN(" DIRECTORY='"));
#ifdef __WIN__
    /* Convert \ to / to be able to create table on unix */
    char *winfilename= (char*) thd->memdup(filename, length);
    char *pos, *end;
    for (pos= winfilename, end= pos+length ; pos < end ; pos++)
    {
      if (*pos == '\\')
        *pos = '/';
    }
    filename= winfilename;
#endif
    packet->append(filename, length);
    packet->append('\'');
  }
}


#define LIST_PROCESS_HOST_LEN 64

/*
  Build a CREATE TABLE statement for a table.

  SYNOPSIS
    store_create_info()
    thd               The thread
    table_list        A list containing one table to write statement
                      for.
    packet            Pointer to a string where statement will be
                      written.
    create_info_arg   Pointer to create information that can be used
                      to tailor the format of the statement.  Can be
                      NULL, in which case only SQL_MODE is considered
                      when building the statement.

  NOTE
    Currently always return 0, but might return error code in the
    future.

  RETURN
    0       OK
 */

int store_create_info(THD *thd, TABLE_LIST *table_list, String *packet,
                      HA_CREATE_INFO *create_info_arg)
{
  List<Item> field_list;
  char tmp[MAX_FIELD_WIDTH], *for_str, buff[128];
  const char *alias;
  String type(tmp, sizeof(tmp), system_charset_info);
  Field **ptr,*field;
  uint primary_key;
  KEY *key_info;
  TABLE *table= table_list->table;
  handler *file= table->file;
  TABLE_SHARE *share= table->s;
  HA_CREATE_INFO create_info;
  bool show_table_options= FALSE;
  bool foreign_db_mode=  (thd->variables.sql_mode & (MODE_POSTGRESQL |
                                                     MODE_ORACLE |
                                                     MODE_MSSQL |
                                                     MODE_DB2 |
                                                     MODE_MAXDB |
                                                     MODE_ANSI)) != 0;
  bool limited_mysql_mode= (thd->variables.sql_mode & (MODE_NO_FIELD_OPTIONS |
                                                       MODE_MYSQL323 |
                                                       MODE_MYSQL40)) != 0;
  my_bitmap_map *old_map;
  DBUG_ENTER("store_create_info");
  DBUG_PRINT("enter",("table: %s", table->s->table_name.str));

  restore_record(table, s->default_values); // Get empty record

  if (share->tmp_table)
    packet->append(STRING_WITH_LEN("CREATE TEMPORARY TABLE "));
  else
    packet->append(STRING_WITH_LEN("CREATE TABLE "));
  if (create_info_arg &&
      (create_info_arg->options & HA_LEX_CREATE_IF_NOT_EXISTS))
    packet->append(STRING_WITH_LEN("IF NOT EXISTS "));
  if (table_list->schema_table)
    alias= table_list->schema_table->table_name;
  else
  {
    if (lower_case_table_names == 2)
      alias= table->alias;
    else
    {
      alias= share->table_name.str;
    }
  }
  append_identifier(thd, packet, alias, strlen(alias));
  packet->append(STRING_WITH_LEN(" (\n"));
  /*
    We need this to get default values from the table
    We have to restore the read_set if we are called from insert in case
    of row based replication.
  */
  old_map= tmp_use_all_columns(table, table->read_set);

  for (ptr=table->field ; (field= *ptr); ptr++)
  {
    bool has_default;
    bool has_now_default;
    uint flags = field->flags;

    if (ptr != table->field)
      packet->append(STRING_WITH_LEN(",\n"));

    packet->append(STRING_WITH_LEN("  "));
    append_identifier(thd,packet,field->field_name, strlen(field->field_name));
    packet->append(' ');
    // check for surprises from the previous call to Field::sql_type()
    if (type.ptr() != tmp)
      type.set(tmp, sizeof(tmp), system_charset_info);
    else
      type.set_charset(system_charset_info);

    field->sql_type(type);
    packet->append(type.ptr(), type.length(), system_charset_info);

    if (field->has_charset() && 
        !(thd->variables.sql_mode & (MODE_MYSQL323 | MODE_MYSQL40)))
    {
      if (field->charset() != share->table_charset)
      {
	packet->append(STRING_WITH_LEN(" CHARACTER SET "));
	packet->append(field->charset()->csname);
      }
      /* 
	For string types dump collation name only if 
	collation is not primary for the given charset
      */
      if (!(field->charset()->state & MY_CS_PRIMARY))
      {
	packet->append(STRING_WITH_LEN(" COLLATE "));
	packet->append(field->charset()->name);
      }
    }

    if (flags & NOT_NULL_FLAG)
      packet->append(STRING_WITH_LEN(" NOT NULL"));
    else if (field->type() == MYSQL_TYPE_TIMESTAMP)
    {
      /*
        TIMESTAMP field require explicit NULL flag, because unlike
        all other fields they are treated as NOT NULL by default.
      */
      packet->append(STRING_WITH_LEN(" NULL"));
    }

    /* 
      Again we are using CURRENT_TIMESTAMP instead of NOW because it is
      more standard 
    */
    has_now_default= table->timestamp_field == field && 
                     field->unireg_check != Field::TIMESTAMP_UN_FIELD;
    
    has_default= (field->type() != MYSQL_TYPE_BLOB &&
                  !(field->flags & NO_DEFAULT_VALUE_FLAG) &&
		  field->unireg_check != Field::NEXT_NUMBER &&
                  !((thd->variables.sql_mode & (MODE_MYSQL323 | MODE_MYSQL40))
		    && has_now_default));

    if (has_default)
    {
      packet->append(STRING_WITH_LEN(" DEFAULT "));
      if (has_now_default)
        packet->append(STRING_WITH_LEN("CURRENT_TIMESTAMP"));
      else if (!field->is_null())
      {                                             // Not null by default
        type.set(tmp, sizeof(tmp), field->charset());
        field->val_str(&type);
	if (type.length())
	{
	  String def_val;
          uint dummy_errors;
	  /* convert to system_charset_info == utf8 */
	  def_val.copy(type.ptr(), type.length(), field->charset(),
		       system_charset_info, &dummy_errors);
          append_unescaped(packet, def_val.ptr(), def_val.length());
	}
        else
	  packet->append(STRING_WITH_LEN("''"));
      }
      else if (field->maybe_null())
        packet->append(STRING_WITH_LEN("NULL"));    // Null as default
      else
        packet->append(tmp);
    }

    if (!limited_mysql_mode && table->timestamp_field == field && 
        field->unireg_check != Field::TIMESTAMP_DN_FIELD)
      packet->append(STRING_WITH_LEN(" ON UPDATE CURRENT_TIMESTAMP"));

    if (field->unireg_check == Field::NEXT_NUMBER && 
        !(thd->variables.sql_mode & MODE_NO_FIELD_OPTIONS))
      packet->append(STRING_WITH_LEN(" AUTO_INCREMENT"));

    if (field->comment.length)
    {
      packet->append(STRING_WITH_LEN(" COMMENT "));
      append_unescaped(packet, field->comment.str, field->comment.length);
    }
  }

  key_info= table->key_info;
  bzero((char*) &create_info, sizeof(create_info));
  file->update_create_info(&create_info);
  primary_key= share->primary_key;

  for (uint i=0 ; i < share->keys ; i++,key_info++)
  {
    KEY_PART_INFO *key_part= key_info->key_part;
    bool found_primary=0;
    packet->append(STRING_WITH_LEN(",\n  "));

    if (i == primary_key && !strcmp(key_info->name, primary_key_name))
    {
      found_primary=1;
      /*
        No space at end, because a space will be added after where the
        identifier would go, but that is not added for primary key.
      */
      packet->append(STRING_WITH_LEN("PRIMARY KEY"));
    }
    else if (key_info->flags & HA_NOSAME)
      packet->append(STRING_WITH_LEN("UNIQUE KEY "));
    else if (key_info->flags & HA_FULLTEXT)
      packet->append(STRING_WITH_LEN("FULLTEXT KEY "));
    else if (key_info->flags & HA_SPATIAL)
      packet->append(STRING_WITH_LEN("SPATIAL KEY "));
    else
      packet->append(STRING_WITH_LEN("KEY "));

    if (!found_primary)
     append_identifier(thd, packet, key_info->name, strlen(key_info->name));

    packet->append(STRING_WITH_LEN(" ("));

    for (uint j=0 ; j < key_info->key_parts ; j++,key_part++)
    {
      if (j)
        packet->append(',');

      if (key_part->field)
        append_identifier(thd,packet,key_part->field->field_name,
			  strlen(key_part->field->field_name));
      if (key_part->field &&
          (key_part->length !=
           table->field[key_part->fieldnr-1]->key_length() &&
           !(key_info->flags & (HA_FULLTEXT | HA_SPATIAL))))
      {
        char *end;
        buff[0] = '(';
        end= int10_to_str((long) key_part->length /
                          key_part->field->charset()->mbmaxlen,
                          buff + 1,10);
        *end++ = ')';
        packet->append(buff,(uint) (end-buff));
      }
    }
    packet->append(')');
    store_key_options(thd, packet, table, key_info);
    if (key_info->parser)
    {
      LEX_STRING *parser_name= plugin_name(key_info->parser);
      packet->append(STRING_WITH_LEN(" /*!50100 WITH PARSER "));
      append_identifier(thd, packet, parser_name->str, parser_name->length);
      packet->append(STRING_WITH_LEN(" */ "));
    }
  }

  /*
    Get possible foreign key definitions stored in InnoDB and append them
    to the CREATE TABLE statement
  */

  if ((for_str= file->get_foreign_key_create_info()))
  {
    packet->append(for_str, strlen(for_str));
    file->free_foreign_key_create_info(for_str);
  }

  packet->append(STRING_WITH_LEN("\n)"));
  if (!(thd->variables.sql_mode & MODE_NO_TABLE_OPTIONS) && !foreign_db_mode)
  {
    show_table_options= TRUE;
    /*
      Get possible table space definitions and append them
      to the CREATE TABLE statement
    */

    if ((for_str= file->get_tablespace_name(thd,0,0)))
    {
      packet->append(STRING_WITH_LEN(" /*!50100 TABLESPACE "));
      packet->append(for_str, strlen(for_str));
      packet->append(STRING_WITH_LEN(" STORAGE DISK */"));
      my_free(for_str, MYF(0));
    }

    /*
      IF   check_create_info
      THEN add ENGINE only if it was used when creating the table
    */
    if (!create_info_arg ||
        (create_info_arg->used_fields & HA_CREATE_USED_ENGINE))
    {
      if (thd->variables.sql_mode & (MODE_MYSQL323 | MODE_MYSQL40))
        packet->append(STRING_WITH_LEN(" TYPE="));
      else
        packet->append(STRING_WITH_LEN(" ENGINE="));
#ifdef WITH_PARTITION_STORAGE_ENGINE
    if (table->part_info)
      packet->append(ha_resolve_storage_engine_name(
                        table->part_info->default_engine_type));
    else
      packet->append(file->table_type());
#else
      packet->append(file->table_type());
#endif
    }

    /*
      Add AUTO_INCREMENT=... if there is an AUTO_INCREMENT column,
      and NEXT_ID > 1 (the default).  We must not print the clause
      for engines that do not support this as it would break the
      import of dumps, but as of this writing, the test for whether
      AUTO_INCREMENT columns are allowed and wether AUTO_INCREMENT=...
      is supported is identical, !(file->table_flags() & HA_NO_AUTO_INCREMENT))
      Because of that, we do not explicitly test for the feature,
      but may extrapolate its existence from that of an AUTO_INCREMENT column.
    */

    if (create_info.auto_increment_value > 1)
    {
      char *end;
      packet->append(STRING_WITH_LEN(" AUTO_INCREMENT="));
      end= longlong10_to_str(create_info.auto_increment_value, buff,10);
      packet->append(buff, (uint) (end - buff));
    }

    
    if (share->table_charset &&
	!(thd->variables.sql_mode & MODE_MYSQL323) &&
	!(thd->variables.sql_mode & MODE_MYSQL40))
    {
      /*
        IF   check_create_info
        THEN add DEFAULT CHARSET only if it was used when creating the table
      */
      if (!create_info_arg ||
          (create_info_arg->used_fields & HA_CREATE_USED_DEFAULT_CHARSET))
      {
        packet->append(STRING_WITH_LEN(" DEFAULT CHARSET="));
        packet->append(share->table_charset->csname);
        if (!(share->table_charset->state & MY_CS_PRIMARY))
        {
          packet->append(STRING_WITH_LEN(" COLLATE="));
          packet->append(table->s->table_charset->name);
        }
      }
    }

    if (share->min_rows)
    {
      char *end;
      packet->append(STRING_WITH_LEN(" MIN_ROWS="));
      end= longlong10_to_str(share->min_rows, buff, 10);
      packet->append(buff, (uint) (end- buff));
    }

    if (share->max_rows && !table_list->schema_table)
    {
      char *end;
      packet->append(STRING_WITH_LEN(" MAX_ROWS="));
      end= longlong10_to_str(share->max_rows, buff, 10);
      packet->append(buff, (uint) (end - buff));
    }

    if (share->avg_row_length)
    {
      char *end;
      packet->append(STRING_WITH_LEN(" AVG_ROW_LENGTH="));
      end= longlong10_to_str(share->avg_row_length, buff,10);
      packet->append(buff, (uint) (end - buff));
    }

    if (share->db_create_options & HA_OPTION_PACK_KEYS)
      packet->append(STRING_WITH_LEN(" PACK_KEYS=1"));
    if (share->db_create_options & HA_OPTION_NO_PACK_KEYS)
      packet->append(STRING_WITH_LEN(" PACK_KEYS=0"));
    if (share->db_create_options & HA_OPTION_CHECKSUM)
      packet->append(STRING_WITH_LEN(" CHECKSUM=1"));
    if (share->db_create_options & HA_OPTION_DELAY_KEY_WRITE)
      packet->append(STRING_WITH_LEN(" DELAY_KEY_WRITE=1"));
    if (share->row_type != ROW_TYPE_DEFAULT)
    {
      packet->append(STRING_WITH_LEN(" ROW_FORMAT="));
      packet->append(ha_row_type[(uint) share->row_type]);
    }
    if (share->transactional != HA_CHOICE_UNDEF)
    {
      packet->append(STRING_WITH_LEN(" TRANSACTIONAL="));
      packet->append(share->transactional == HA_CHOICE_YES ? "1" : "0", 1);
    }
    if (table->s->key_block_size)
    {
      char *end;
      packet->append(STRING_WITH_LEN(" KEY_BLOCK_SIZE="));
      end= longlong10_to_str(table->s->key_block_size, buff, 10);
      packet->append(buff, (uint) (end - buff));
    }
    table->file->append_create_info(packet);
    if (share->comment.length)
    {
      packet->append(STRING_WITH_LEN(" COMMENT="));
      append_unescaped(packet, share->comment.str, share->comment.length);
    }
    if (share->connect_string.length)
    {
      packet->append(STRING_WITH_LEN(" CONNECTION="));
      append_unescaped(packet, share->connect_string.str, share->connect_string.length);
    }
    append_directory(thd, packet, "DATA",  create_info.data_file_name);
    append_directory(thd, packet, "INDEX", create_info.index_file_name);
  }
#ifdef WITH_PARTITION_STORAGE_ENGINE
  {
    /*
      Partition syntax for CREATE TABLE is at the end of the syntax.
    */
    uint part_syntax_len;
    char *part_syntax;
    if (table->part_info &&
        (!table->part_info->is_auto_partitioned) &&
        ((part_syntax= generate_partition_syntax(table->part_info,
                                                  &part_syntax_len,
                                                  FALSE,
                                                  show_table_options))))
    {
       packet->append(STRING_WITH_LEN(" /*!50100"));
       packet->append(part_syntax, part_syntax_len);
       packet->append(STRING_WITH_LEN(" */"));
       my_free(part_syntax, MYF(0));
    }
  }
#endif
  tmp_restore_column_map(table->read_set, old_map);
  DBUG_RETURN(0);
}


static void store_key_options(THD *thd, String *packet, TABLE *table,
                              KEY *key_info)
{
  bool limited_mysql_mode= (thd->variables.sql_mode &
                            (MODE_NO_FIELD_OPTIONS | MODE_MYSQL323 |
                             MODE_MYSQL40)) != 0;
  bool foreign_db_mode=  (thd->variables.sql_mode & (MODE_POSTGRESQL |
                                                     MODE_ORACLE |
                                                     MODE_MSSQL |
                                                     MODE_DB2 |
                                                     MODE_MAXDB |
                                                     MODE_ANSI)) != 0;
  char *end, buff[32];

  if (!(thd->variables.sql_mode & MODE_NO_KEY_OPTIONS) &&
      !limited_mysql_mode && !foreign_db_mode)
  {

    if (key_info->algorithm == HA_KEY_ALG_BTREE)
      packet->append(STRING_WITH_LEN(" USING BTREE"));

    if (key_info->algorithm == HA_KEY_ALG_HASH)
      packet->append(STRING_WITH_LEN(" USING HASH"));

    /* send USING only in non-default case: non-spatial rtree */
    if ((key_info->algorithm == HA_KEY_ALG_RTREE) &&
        !(key_info->flags & HA_SPATIAL))
      packet->append(STRING_WITH_LEN(" USING RTREE"));

    if ((key_info->flags & HA_USES_BLOCK_SIZE) &&
        table->s->key_block_size != key_info->block_size)
    {
      packet->append(STRING_WITH_LEN(" KEY_BLOCK_SIZE="));
      end= longlong10_to_str(key_info->block_size, buff, 10);
      packet->append(buff, (uint) (end - buff));
    }
  }
}


void
view_store_options(THD *thd, TABLE_LIST *table, String *buff)
{
  append_algorithm(table, buff);
  append_definer(thd, buff, &table->definer.user, &table->definer.host);
  if (table->view_suid)
    buff->append(STRING_WITH_LEN("SQL SECURITY DEFINER "));
  else
    buff->append(STRING_WITH_LEN("SQL SECURITY INVOKER "));
}


/*
  Append DEFINER clause to the given buffer.
  
  SYNOPSIS
    append_definer()
    thd           [in] thread handle
    buffer        [inout] buffer to hold DEFINER clause
    definer_user  [in] user name part of definer
    definer_host  [in] host name part of definer
*/

static void append_algorithm(TABLE_LIST *table, String *buff)
{
  buff->append(STRING_WITH_LEN("ALGORITHM="));
  switch ((int8)table->algorithm) {
  case VIEW_ALGORITHM_UNDEFINED:
    buff->append(STRING_WITH_LEN("UNDEFINED "));
    break;
  case VIEW_ALGORITHM_TMPTABLE:
    buff->append(STRING_WITH_LEN("TEMPTABLE "));
    break;
  case VIEW_ALGORITHM_MERGE:
    buff->append(STRING_WITH_LEN("MERGE "));
    break;
  default:
    DBUG_ASSERT(0); // never should happen
  }
}

/*
  Append DEFINER clause to the given buffer.
  
  SYNOPSIS
    append_definer()
    thd           [in] thread handle
    buffer        [inout] buffer to hold DEFINER clause
    definer_user  [in] user name part of definer
    definer_host  [in] host name part of definer
*/

void append_definer(THD *thd, String *buffer, const LEX_STRING *definer_user,
                    const LEX_STRING *definer_host)
{
  buffer->append(STRING_WITH_LEN("DEFINER="));
  append_identifier(thd, buffer, definer_user->str, definer_user->length);
  buffer->append('@');
  append_identifier(thd, buffer, definer_host->str, definer_host->length);
  buffer->append(' ');
}


int
view_store_create_info(THD *thd, TABLE_LIST *table, String *buff)
{
  my_bool foreign_db_mode= (thd->variables.sql_mode & (MODE_POSTGRESQL |
                                                       MODE_ORACLE |
                                                       MODE_MSSQL |
                                                       MODE_DB2 |
                                                       MODE_MAXDB |
                                                       MODE_ANSI)) != 0;
  /*
     Compact output format for view can be used
     - if user has db of this view as current db
     - if this view only references table inside it's own db
  */
  if (!thd->db || strcmp(thd->db, table->view_db.str))
    table->compact_view_format= FALSE;
  else
  {
    TABLE_LIST *tbl;
    table->compact_view_format= TRUE;
    for (tbl= thd->lex->query_tables;
         tbl;
         tbl= tbl->next_global)
    {
      if (strcmp(table->view_db.str, tbl->view ? tbl->view_db.str :tbl->db)!= 0)
      {
        table->compact_view_format= FALSE;
        break;
      }
    }
  }

  buff->append(STRING_WITH_LEN("CREATE "));
  if (!foreign_db_mode)
  {
    view_store_options(thd, table, buff);
  }
  buff->append(STRING_WITH_LEN("VIEW "));
  if (!table->compact_view_format)
  {
    append_identifier(thd, buff, table->view_db.str, table->view_db.length);
    buff->append('.');
  }
  append_identifier(thd, buff, table->view_name.str, table->view_name.length);
  buff->append(STRING_WITH_LEN(" AS "));

  /*
    We can't just use table->query, because our SQL_MODE may trigger
    a different syntax, like when ANSI_QUOTES is defined.
  */
  table->view->unit.print(buff);

  if (table->with_check != VIEW_CHECK_NONE)
  {
    if (table->with_check == VIEW_CHECK_LOCAL)
      buff->append(STRING_WITH_LEN(" WITH LOCAL CHECK OPTION"));
    else
      buff->append(STRING_WITH_LEN(" WITH CASCADED CHECK OPTION"));
  }
  return 0;
}


/****************************************************************************
  Return info about all processes
  returns for each thread: thread id, user, host, db, command, info
****************************************************************************/

class thread_info :public ilink {
public:
  static void *operator new(size_t size)
  {
    return (void*) sql_alloc((uint) size);
  }
  static void operator delete(void *ptr __attribute__((unused)),
                              size_t size __attribute__((unused)))
  { TRASH(ptr, size); }

  ulong thread_id;
  time_t start_time;
  uint   command;
  const char *user,*host,*db,*proc_info,*state_info;
  char *query;
};

#ifdef HAVE_EXPLICIT_TEMPLATE_INSTANTIATION
template class I_List<thread_info>;
#endif

void mysqld_list_processes(THD *thd,const char *user, bool verbose)
{
  Item *field;
  List<Item> field_list;
  I_List<thread_info> thread_infos;
  ulong max_query_length= (verbose ? thd->variables.max_allowed_packet :
			   PROCESS_LIST_WIDTH);
  Protocol *protocol= thd->protocol;
  DBUG_ENTER("mysqld_list_processes");

  field_list.push_back(new Item_int("Id", 0, MY_INT32_NUM_DECIMAL_DIGITS));
  field_list.push_back(new Item_empty_string("User",16));
  field_list.push_back(new Item_empty_string("Host",LIST_PROCESS_HOST_LEN));
  field_list.push_back(field=new Item_empty_string("db",NAME_CHAR_LEN));
  field->maybe_null=1;
  field_list.push_back(new Item_empty_string("Command",16));
  field_list.push_back(new Item_return_int("Time",7, MYSQL_TYPE_LONG));
  field_list.push_back(field=new Item_empty_string("State",30));
  field->maybe_null=1;
  field_list.push_back(field=new Item_empty_string("Info",max_query_length));
  field->maybe_null=1;
  if (protocol->send_fields(&field_list,
                            Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF))
    DBUG_VOID_RETURN;

  VOID(pthread_mutex_lock(&LOCK_thread_count)); // For unlink from list
  if (!thd->killed)
  {
    I_List_iterator<THD> it(threads);
    THD *tmp;
    while ((tmp=it++))
    {
      Security_context *tmp_sctx= tmp->security_ctx;
      struct st_my_thread_var *mysys_var;
      if ((tmp->vio_ok() || tmp->system_thread) &&
          (!user || (tmp_sctx->user && !strcmp(tmp_sctx->user, user))))
      {
        thread_info *thd_info= new thread_info;

        thd_info->thread_id=tmp->thread_id;
        thd_info->user= thd->strdup(tmp_sctx->user ? tmp_sctx->user :
                                    (tmp->system_thread ?
                                     "system user" : "unauthenticated user"));
	if (tmp->peer_port && (tmp_sctx->host || tmp_sctx->ip) &&
            thd->security_ctx->host_or_ip[0])
	{
	  if ((thd_info->host= (char*) thd->alloc(LIST_PROCESS_HOST_LEN+1)))
	    my_snprintf((char *) thd_info->host, LIST_PROCESS_HOST_LEN,
			"%s:%u", tmp_sctx->host_or_ip, tmp->peer_port);
	}
	else
	  thd_info->host= thd->strdup(tmp_sctx->host_or_ip[0] ? 
                                      tmp_sctx->host_or_ip : 
                                      tmp_sctx->host ? tmp_sctx->host : "");
        if ((thd_info->db=tmp->db))             // Safe test
          thd_info->db=thd->strdup(thd_info->db);
        thd_info->command=(int) tmp->command;
        if ((mysys_var= tmp->mysys_var))
          pthread_mutex_lock(&mysys_var->mutex);
        thd_info->proc_info= (char*) (tmp->killed == THD::KILL_CONNECTION? "Killed" : 0);
#ifndef EMBEDDED_LIBRARY
        thd_info->state_info= (char*) (tmp->locked ? "Locked" :
                                       tmp->net.reading_or_writing ?
                                       (tmp->net.reading_or_writing == 2 ?
                                        "Writing to net" :
                                        thd_info->command == COM_SLEEP ? "" :
                                        "Reading from net") :
                                       tmp->proc_info ? tmp->proc_info :
                                       tmp->mysys_var &&
                                       tmp->mysys_var->current_cond ?
                                       "Waiting on cond" : NullS);
#else
        thd_info->state_info= (char*)"Writing to net";
#endif
        if (mysys_var)
          pthread_mutex_unlock(&mysys_var->mutex);

#ifdef EXTRA_DEBUG
        thd_info->start_time= tmp->time_after_lock;
#else
        thd_info->start_time= tmp->start_time;
#endif
        thd_info->query=0;
        if (tmp->query)
        {
	  /* 
            query_length is always set to 0 when we set query = NULL; see
	    the comment in sql_class.h why this prevents crashes in possible
            races with query_length
          */
          uint length= min(max_query_length, tmp->query_length);
          thd_info->query=(char*) thd->strmake(tmp->query,length);
        }
        thread_infos.append(thd_info);
      }
    }
  }
  VOID(pthread_mutex_unlock(&LOCK_thread_count));

  thread_info *thd_info;
  time_t now= time(0);
  while ((thd_info=thread_infos.get()))
  {
    protocol->prepare_for_resend();
    protocol->store((ulonglong) thd_info->thread_id);
    protocol->store(thd_info->user, system_charset_info);
    protocol->store(thd_info->host, system_charset_info);
    protocol->store(thd_info->db, system_charset_info);
    if (thd_info->proc_info)
      protocol->store(thd_info->proc_info, system_charset_info);
    else
      protocol->store(command_name[thd_info->command].str, system_charset_info);
    if (thd_info->start_time)
      protocol->store((uint32) (now - thd_info->start_time));
    else
      protocol->store_null();
    protocol->store(thd_info->state_info, system_charset_info);
    protocol->store(thd_info->query, system_charset_info);
    if (protocol->write())
      break; /* purecov: inspected */
  }
  send_eof(thd);
  DBUG_VOID_RETURN;
}

int fill_schema_processlist(THD* thd, TABLE_LIST* tables, COND* cond)
{
  TABLE *table= tables->table;
  CHARSET_INFO *cs= system_charset_info;
  char *user;
  time_t now= time(0);
  DBUG_ENTER("fill_process_list");

  user= thd->security_ctx->master_access & PROCESS_ACL ?
        NullS : thd->security_ctx->priv_user;

  VOID(pthread_mutex_lock(&LOCK_thread_count));

  if (!thd->killed)
  {
    I_List_iterator<THD> it(threads);
    THD* tmp;

    while ((tmp= it++))
    {
      Security_context *tmp_sctx= tmp->security_ctx;
      struct st_my_thread_var *mysys_var;
      const char *val;

      if ((!tmp->vio_ok() && !tmp->system_thread) ||
          (user && (!tmp_sctx->user || strcmp(tmp_sctx->user, user))))
        continue;

      restore_record(table, s->default_values);
      /* ID */
      table->field[0]->store((longlong) tmp->thread_id, TRUE);
      /* USER */
      val= tmp_sctx->user ? tmp_sctx->user :
            (tmp->system_thread ? "system user" : "unauthenticated user");
      table->field[1]->store(val, strlen(val), cs);
      /* HOST */
      if (tmp->peer_port && (tmp_sctx->host || tmp_sctx->ip) &&
          thd->security_ctx->host_or_ip[0])
      {
        char host[LIST_PROCESS_HOST_LEN + 1];
        my_snprintf(host, LIST_PROCESS_HOST_LEN, "%s:%u",
                    tmp_sctx->host_or_ip, tmp->peer_port);
        table->field[2]->store(host, strlen(host), cs);
      }
      else
        table->field[2]->store(tmp_sctx->host_or_ip,
                               strlen(tmp_sctx->host_or_ip), cs);
      /* DB */
      if (tmp->db)
      {
        table->field[3]->store(tmp->db, strlen(tmp->db), cs);
        table->field[3]->set_notnull();
      }

      if ((mysys_var= tmp->mysys_var))
        pthread_mutex_lock(&mysys_var->mutex);
      /* COMMAND */
      if ((val= (char *) (tmp->killed == THD::KILL_CONNECTION? "Killed" : 0)))
        table->field[4]->store(val, strlen(val), cs);
      else
        table->field[4]->store(command_name[tmp->command].str,
                               command_name[tmp->command].length, cs);
      /* MYSQL_TIME */
      table->field[5]->store((uint32)(tmp->start_time ?
                                      now - tmp->start_time : 0), TRUE);
      /* STATE */
#ifndef EMBEDDED_LIBRARY
      val= (char*) (tmp->locked ? "Locked" :
                    tmp->net.reading_or_writing ?
                    (tmp->net.reading_or_writing == 2 ?
                     "Writing to net" :
                     tmp->command == COM_SLEEP ? "" :
                     "Reading from net") :
                    tmp->proc_info ? tmp->proc_info :
                    tmp->mysys_var &&
                    tmp->mysys_var->current_cond ?
                    "Waiting on cond" : NullS);
#else
      val= (char *) "Writing to net";
#endif
      if (val)
      {
        table->field[6]->store(val, strlen(val), cs);
        table->field[6]->set_notnull();
      }

      if (mysys_var)
        pthread_mutex_unlock(&mysys_var->mutex);

      /* INFO */
      if (tmp->query)
      {
        table->field[7]->store(tmp->query,
                               min(PROCESS_LIST_INFO_WIDTH,
                                   tmp->query_length), cs);
        table->field[7]->set_notnull();
      }

      if (schema_table_store_record(thd, table))
      {
        VOID(pthread_mutex_unlock(&LOCK_thread_count));
        DBUG_RETURN(1);
      }
    }
  }

  VOID(pthread_mutex_unlock(&LOCK_thread_count));
  DBUG_RETURN(0);
}

/*****************************************************************************
  Status functions
*****************************************************************************/

static DYNAMIC_ARRAY all_status_vars;
static bool status_vars_inited= 0;
static int show_var_cmp(const void *var1, const void *var2)
{
  return strcmp(((SHOW_VAR*)var1)->name, ((SHOW_VAR*)var2)->name);
}

/*
  deletes all the SHOW_UNDEF elements from the array and calls
  delete_dynamic() if it's completely empty.
*/
static void shrink_var_array(DYNAMIC_ARRAY *array)
{
  uint a,b;
  SHOW_VAR *all= dynamic_element(array, 0, SHOW_VAR *);

  for (a= b= 0; b < array->elements; b++)
    if (all[b].type != SHOW_UNDEF)
      all[a++]= all[b];
  if (a)
  {
    bzero(all+a, sizeof(SHOW_VAR)); // writing NULL-element to the end
    array->elements= a;
  }
  else // array is completely empty - delete it
    delete_dynamic(array);
}

/*
  Adds an array of SHOW_VAR entries to the output of SHOW STATUS

  SYNOPSIS
    add_status_vars(SHOW_VAR *list)
    list - an array of SHOW_VAR entries to add to all_status_vars
           the last entry must be {0,0,SHOW_UNDEF}

  NOTE
    The handling of all_status_vars[] is completely internal, it's allocated
    automatically when something is added to it, and deleted completely when
    the last entry is removed.

    As a special optimization, if add_status_vars() is called before
    init_status_vars(), it assumes "startup mode" - neither concurrent access
    to the array nor SHOW STATUS are possible (thus it skips locks and qsort)

    The last entry of the all_status_vars[] should always be {0,0,SHOW_UNDEF}
*/
int add_status_vars(SHOW_VAR *list)
{
  int res= 0;
  if (status_vars_inited)
    pthread_mutex_lock(&LOCK_status);
  if (!all_status_vars.buffer && // array is not allocated yet - do it now
      my_init_dynamic_array(&all_status_vars, sizeof(SHOW_VAR), 200, 20))
  {
    res= 1;
    goto err;
  }
  while (list->name)
    res|= insert_dynamic(&all_status_vars, (uchar*)list++);
  res|= insert_dynamic(&all_status_vars, (uchar*)list); // appending NULL-element
  all_status_vars.elements--; // but next insert_dynamic should overwite it
  if (status_vars_inited)
    sort_dynamic(&all_status_vars, show_var_cmp);
err:
  if (status_vars_inited)
    pthread_mutex_unlock(&LOCK_status);
  return res;
}

/*
  Make all_status_vars[] usable for SHOW STATUS

  NOTE
    See add_status_vars(). Before init_status_vars() call, add_status_vars()
    works in a special fast "startup" mode. Thus init_status_vars()
    should be called as late as possible but before enabling multi-threading.
*/
void init_status_vars()
{
  status_vars_inited=1;
  sort_dynamic(&all_status_vars, show_var_cmp);
}

void reset_status_vars()
{
  SHOW_VAR *ptr= (SHOW_VAR*) all_status_vars.buffer;
  SHOW_VAR *last= ptr + all_status_vars.elements;
  for (; ptr < last; ptr++)
  {
    /* Note that SHOW_LONG_NOFLUSH variables are not reset */
    if (ptr->type == SHOW_LONG)
      *(ulong*) ptr->value= 0;
  }  
}

/*
  catch-all cleanup function, cleans up everything no matter what

  DESCRIPTION
    This function is not strictly required if all add_to_status/
    remove_status_vars are properly paired, but it's a safety measure that
    deletes everything from the all_status_vars[] even if some
    remove_status_vars were forgotten
*/
void free_status_vars()
{
  delete_dynamic(&all_status_vars);
}

/*
  Removes an array of SHOW_VAR entries from the output of SHOW STATUS

  SYNOPSIS
    remove_status_vars(SHOW_VAR *list)
    list - an array of SHOW_VAR entries to remove to all_status_vars
           the last entry must be {0,0,SHOW_UNDEF}

  NOTE
    there's lots of room for optimizing this, especially in non-sorted mode,
    but nobody cares - it may be called only in case of failed plugin
    initialization in the mysqld startup.
*/

void remove_status_vars(SHOW_VAR *list)
{
  if (status_vars_inited)
  {
    pthread_mutex_lock(&LOCK_status);
    SHOW_VAR *all= dynamic_element(&all_status_vars, 0, SHOW_VAR *);
    int a= 0, b= all_status_vars.elements, c= (a+b)/2;

    for (; list->name; list++)
    {
      int res= 0;
      for (a= 0, b= all_status_vars.elements; b-a > 1; c= (a+b)/2)
      {
        res= show_var_cmp(list, all+c);
        if (res < 0)
          b= c;
        else if (res > 0)
          a= c;
        else
          break;
      }
      if (res == 0)
        all[c].type= SHOW_UNDEF;
    }
    shrink_var_array(&all_status_vars);
    pthread_mutex_unlock(&LOCK_status);
  }
  else
  {
    SHOW_VAR *all= dynamic_element(&all_status_vars, 0, SHOW_VAR *);
    uint i;
    for (; list->name; list++)
    {
      for (i= 0; i < all_status_vars.elements; i++)
      {
        if (show_var_cmp(list, all+i))
          continue;
        all[i].type= SHOW_UNDEF;
        break;
      }
    }
    shrink_var_array(&all_status_vars);
  }
}

inline void make_upper(char *buf)
{
  for (; *buf; buf++)
    *buf= my_toupper(system_charset_info, *buf);
}

static bool show_status_array(THD *thd, const char *wild,
                              SHOW_VAR *variables,
                              enum enum_var_type value_type,
                              struct system_status_var *status_var,
                              const char *prefix, TABLE *table,
                              bool ucase_names)
{
  MY_ALIGNED_BYTE_ARRAY(buff_data, SHOW_VAR_FUNC_BUFF_SIZE, long);
  char * const buff= (char *) &buff_data;
  char *prefix_end;
  /* the variable name should not be longer than 64 characters */
  char name_buffer[64];
  int len;
  LEX_STRING null_lex_str;
  SHOW_VAR tmp, *var;
  DBUG_ENTER("show_status_array");

  null_lex_str.str= 0;				// For sys_var->value_ptr()
  null_lex_str.length= 0;

  prefix_end=strnmov(name_buffer, prefix, sizeof(name_buffer)-1);
  if (*prefix)
    *prefix_end++= '_';
  len=name_buffer + sizeof(name_buffer) - prefix_end;

  for (; variables->name; variables++)
  {
    strnmov(prefix_end, variables->name, len);
    name_buffer[sizeof(name_buffer)-1]=0;       /* Safety */
    if (ucase_names)
      make_upper(name_buffer);

    /*
      if var->type is SHOW_FUNC, call the function.
      Repeat as necessary, if new var is again SHOW_FUNC
    */
    for (var=variables; var->type == SHOW_FUNC; var= &tmp)
      ((mysql_show_var_func)(var->value))(thd, &tmp, buff);

    SHOW_TYPE show_type=var->type;
    if (show_type == SHOW_ARRAY)
    {
      show_status_array(thd, wild, (SHOW_VAR *) var->value, value_type,
                        status_var, name_buffer, table, ucase_names);
    }
    else
    {
      if (!(wild && wild[0] && wild_case_compare(system_charset_info,
                                                 name_buffer, wild)))
      {
        char *value=var->value;
        const char *pos, *end;                  // We assign a lot of const's

        pthread_mutex_lock(&LOCK_global_system_variables);

        if (show_type == SHOW_SYS)
        {
          show_type= ((sys_var*) value)->show_type();
          value=     (char*) ((sys_var*) value)->value_ptr(thd, value_type,
                                                           &null_lex_str);
        }

        pos= end= buff;
        /*
          note that value may be == buff. All SHOW_xxx code below
          should still work in this case
        */
        switch (show_type) {
        case SHOW_DOUBLE_STATUS:
        {
          value= ((char *) status_var + (ulong) value);
          end= buff + sprintf(buff, "%f", *(double*) value);
          break;
        }
        case SHOW_LONG_STATUS:
          value= ((char *) status_var + (ulong) value);
          /* fall through */
        case SHOW_LONG:
        case SHOW_LONG_NOFLUSH: // the difference lies in refresh_status()
          end= int10_to_str(*(long*) value, buff, 10);
          break;
        case SHOW_LONGLONG_STATUS:
          value= ((char *) status_var + (ulonglong) value);
        case SHOW_LONGLONG:
          end= longlong10_to_str(*(longlong*) value, buff, 10);
          break;
        case SHOW_HA_ROWS:
          end= longlong10_to_str((longlong) *(ha_rows*) value, buff, 10);
          break;
        case SHOW_BOOL:
          end= strmov(buff, *(bool*) value ? "ON" : "OFF");
          break;
        case SHOW_MY_BOOL:
          end= strmov(buff, *(my_bool*) value ? "ON" : "OFF");
          break;
        case SHOW_INT:
          end= int10_to_str((long) *(uint32*) value, buff, 10);
          break;
        case SHOW_HAVE:
        {
          SHOW_COMP_OPTION tmp= *(SHOW_COMP_OPTION*) value;
          pos= show_comp_option_name[(int) tmp];
          end= strend(pos);
          break;
        }
        case SHOW_CHAR:
        {
          if (!(pos= value))
            pos= "";
          end= strend(pos);
          break;
        }
       case SHOW_CHAR_PTR:
        {
          if (!(pos= *(char**) value))
            pos= "";
          end= strend(pos);
          break;
        }
        case SHOW_KEY_CACHE_LONG:
          value= (char*) dflt_key_cache + (ulong)value;
          end= int10_to_str(*(long*) value, buff, 10);
          break;
        case SHOW_KEY_CACHE_LONGLONG:
          value= (char*) dflt_key_cache + (ulong)value;
	  end= longlong10_to_str(*(longlong*) value, buff, 10);
	  break;
        case SHOW_UNDEF:
          break;                                        // Return empty string
        case SHOW_SYS:                                  // Cannot happen
        default:
          DBUG_ASSERT(0);
          break;
        }
        restore_record(table, s->default_values);
        table->field[0]->store(name_buffer, strlen(name_buffer),
                               system_charset_info);
        table->field[1]->store(pos, (uint32) (end - pos), system_charset_info);
        table->field[1]->set_notnull();

        pthread_mutex_unlock(&LOCK_global_system_variables);

        if (schema_table_store_record(thd, table))
          DBUG_RETURN(TRUE);
      }
    }
  }

  DBUG_RETURN(FALSE);
}


/* collect status for all running threads */

void calc_sum_of_all_status(STATUS_VAR *to)
{
  DBUG_ENTER("calc_sum_of_all_status");

  /* Ensure that thread id not killed during loop */
  VOID(pthread_mutex_lock(&LOCK_thread_count)); // For unlink from list

  I_List_iterator<THD> it(threads);
  THD *tmp;
  
  /* Get global values as base */
  *to= global_status_var;
  
  /* Add to this status from existing threads */
  while ((tmp= it++))
    add_to_status(to, &tmp->status_var);
  
  VOID(pthread_mutex_unlock(&LOCK_thread_count));
  DBUG_VOID_RETURN;
}


LEX_STRING *make_lex_string(THD *thd, LEX_STRING *lex_str,
                            const char* str, uint length,
                            bool allocate_lex_string)
{
  MEM_ROOT *mem= thd->mem_root;
  if (allocate_lex_string)
    if (!(lex_str= (LEX_STRING *)thd->alloc(sizeof(LEX_STRING))))
      return 0;
  lex_str->str= strmake_root(mem, str, length);
  lex_str->length= length;
  return lex_str;
}


/* INFORMATION_SCHEMA name */
LEX_STRING INFORMATION_SCHEMA_NAME= { C_STRING_WITH_LEN("information_schema")};

/* This is only used internally, but we need it here as a forward reference */
extern ST_SCHEMA_TABLE schema_tables[];

typedef struct st_index_field_values
{
  const char *db_value, *table_value;
} INDEX_FIELD_VALUES;


/*
  Store record to I_S table, convert HEAP table
  to MyISAM if necessary

  SYNOPSIS
    schema_table_store_record()
    thd                   thread handler
    table                 Information schema table to be updated

  RETURN
    0	                  success
    1	                  error
*/

bool schema_table_store_record(THD *thd, TABLE *table)
{
  int error;
  if ((error= table->file->ha_write_row(table->record[0])))
  {
    if (create_myisam_from_heap(thd, table, 
                                table->pos_in_table_list->schema_table_param,
                                error, 0))
      return 1;
  }
  return 0;
}


void get_index_field_values(LEX *lex, INDEX_FIELD_VALUES *index_field_values)
{
  const char *wild= lex->wild ? lex->wild->ptr() : NullS;
  switch (lex->sql_command) {
  case SQLCOM_SHOW_DATABASES:
    index_field_values->db_value= wild;
    break;
  case SQLCOM_SHOW_TABLES:
  case SQLCOM_SHOW_TABLE_STATUS:
  case SQLCOM_SHOW_TRIGGERS:
  case SQLCOM_SHOW_EVENTS:
    index_field_values->db_value= lex->select_lex.db;
    index_field_values->table_value= wild;
    break;
  default:
    index_field_values->db_value= NullS;
    index_field_values->table_value= NullS;
    break;
  }
}


int make_table_list(THD *thd, SELECT_LEX *sel,
                    char *db, char *table)
{
  Table_ident *table_ident;
  LEX_STRING ident_db, ident_table;
  ident_db.str= db; 
  ident_db.length= strlen(db);
  ident_table.str= table;
  ident_table.length= strlen(table);
  table_ident= new Table_ident(thd, ident_db, ident_table, 1);
  sel->init_query();
  if (!sel->add_table_to_list(thd, table_ident, 0, 0, TL_READ))
    return 1;
  return 0;
}


bool uses_only_table_name_fields(Item *item, TABLE_LIST *table)
{
  if (item->type() == Item::FUNC_ITEM)
  {
    Item_func *item_func= (Item_func*)item;
    Item **child;
    Item **item_end= (item_func->arguments()) + item_func->argument_count();
    for (child= item_func->arguments(); child != item_end; child++)
    {
      if (!uses_only_table_name_fields(*child, table))
        return 0;
    }
  }
  else if (item->type() == Item::FIELD_ITEM)
  {
    Item_field *item_field= (Item_field*)item;
    CHARSET_INFO *cs= system_charset_info;
    ST_SCHEMA_TABLE *schema_table= table->schema_table;
    ST_FIELD_INFO *field_info= schema_table->fields_info;
    const char *field_name1= schema_table->idx_field1 >= 0 ? field_info[schema_table->idx_field1].field_name : "";
    const char *field_name2= schema_table->idx_field2 >= 0 ? field_info[schema_table->idx_field2].field_name : "";
    if (table->table != item_field->field->table ||
        (cs->coll->strnncollsp(cs, (uchar *) field_name1, strlen(field_name1),
                               (uchar *) item_field->field_name, 
                               strlen(item_field->field_name), 0) &&
         cs->coll->strnncollsp(cs, (uchar *) field_name2, strlen(field_name2),
                               (uchar *) item_field->field_name, 
                               strlen(item_field->field_name), 0)))
      return 0;
  }
  else if (item->type() == Item::REF_ITEM)
    return uses_only_table_name_fields(item->real_item(), table);
  if (item->type() == Item::SUBSELECT_ITEM &&
      !item->const_item())
    return 0;

  return 1;
}


static COND * make_cond_for_info_schema(COND *cond, TABLE_LIST *table)
{
  if (!cond)
    return (COND*) 0;
  if (cond->type() == Item::COND_ITEM)
  {
    if (((Item_cond*) cond)->functype() == Item_func::COND_AND_FUNC)
    {
      /* Create new top level AND item */
      Item_cond_and *new_cond=new Item_cond_and;
      if (!new_cond)
	return (COND*) 0;
      List_iterator<Item> li(*((Item_cond*) cond)->argument_list());
      Item *item;
      while ((item=li++))
      {
	Item *fix= make_cond_for_info_schema(item, table);
	if (fix)
	  new_cond->argument_list()->push_back(fix);
      }
      switch (new_cond->argument_list()->elements) {
      case 0:
	return (COND*) 0;
      case 1:
	return new_cond->argument_list()->head();
      default:
	new_cond->quick_fix_field();
	return new_cond;
      }
    }
    else
    {						// Or list
      Item_cond_or *new_cond=new Item_cond_or;
      if (!new_cond)
	return (COND*) 0;
      List_iterator<Item> li(*((Item_cond*) cond)->argument_list());
      Item *item;
      while ((item=li++))
      {
	Item *fix=make_cond_for_info_schema(item, table);
	if (!fix)
	  return (COND*) 0;
	new_cond->argument_list()->push_back(fix);
      }
      new_cond->quick_fix_field();
      new_cond->top_level_item();
      return new_cond;
    }
  }

  if (!uses_only_table_name_fields(cond, table))
    return (COND*) 0;
  return cond;
}


enum enum_schema_tables get_schema_table_idx(ST_SCHEMA_TABLE *schema_table)
{
  return (enum enum_schema_tables) (schema_table - &schema_tables[0]);
}


/*
  Create db names list. Information schema name always is first in list

  SYNOPSIS
    make_db_list()
    thd                   thread handler
    files                 list of db names
    wild                  wild string
    idx_field_vals        idx_field_vals->db_name contains db name or
                          wild string
    with_i_schema         returns 1 if we added 'IS' name to list
                          otherwise returns 0
    is_wild_value         if value is 1 then idx_field_vals->db_name is
                          wild string otherwise it's db name; 

  RETURN
    zero                  success
    non-zero              error
*/

int make_db_list(THD *thd, List<char> *files,
                 INDEX_FIELD_VALUES *idx_field_vals,
                 bool *with_i_schema, bool is_wild_value)
{
  LEX *lex= thd->lex;
  *with_i_schema= 0;
  get_index_field_values(lex, idx_field_vals);

  if (is_wild_value)
  {
    /*
      This part of code is only for SHOW DATABASES command.
      idx_field_vals->db_value can be 0 when we don't use
      LIKE clause (see also get_index_field_values() function)
    */
    if (!idx_field_vals->db_value ||
        !wild_case_compare(system_charset_info, 
                           INFORMATION_SCHEMA_NAME.str,
                           idx_field_vals->db_value))
    {
      *with_i_schema= 1;
      if (files->push_back(thd->strdup(INFORMATION_SCHEMA_NAME.str)))
        return 1;
    }
    return (find_files(thd, files, NullS, mysql_data_home,
                       idx_field_vals->db_value, 1) != FIND_FILES_OK);
  }

  /*
    This part of code is for SHOW TABLES, SHOW TABLE STATUS commands.
    idx_field_vals->db_value can't be 0 (see get_index_field_values()
    function).
  */
  if (sql_command_flags[lex->sql_command] & CF_STATUS_COMMAND)
  {
    if (!my_strcasecmp(system_charset_info, INFORMATION_SCHEMA_NAME.str,
                       idx_field_vals->db_value))
    {
      *with_i_schema= 1;
      return files->push_back(thd->strdup(INFORMATION_SCHEMA_NAME.str));
    }
    return files->push_back(thd->strdup(idx_field_vals->db_value));
  }

  /*
    Create list of existing databases. It is used in case
    of select from information schema table
  */
  if (files->push_back(thd->strdup(INFORMATION_SCHEMA_NAME.str)))
    return 1;
  *with_i_schema= 1;
  return (find_files(thd, files, NullS,
                     mysql_data_home, NullS, 1) != FIND_FILES_OK);
}

struct st_add_schema_table 
{
  List<char> *files;
  const char *wild;
};

static my_bool add_schema_table(THD *thd, plugin_ref plugin,
                                void* p_data)
{
  st_add_schema_table *data= (st_add_schema_table *)p_data;
  List<char> *file_list= data->files;
  const char *wild= data->wild;
  ST_SCHEMA_TABLE *schema_table= plugin_data(plugin, ST_SCHEMA_TABLE *);
  DBUG_ENTER("add_schema_table");

  if (schema_table->hidden)
      DBUG_RETURN(0);
  if (wild)
  {
    if (lower_case_table_names)
    {
      if (wild_case_compare(files_charset_info,
                            schema_table->table_name,
                            wild))
        DBUG_RETURN(0);
    }
    else if (wild_compare(schema_table->table_name, wild, 0))
      DBUG_RETURN(0);
  }

  if (file_list->push_back(thd->strdup(schema_table->table_name)))
    DBUG_RETURN(1);

  DBUG_RETURN(0);
}

int schema_tables_add(THD *thd, List<char> *files, const char *wild)
{
  ST_SCHEMA_TABLE *tmp_schema_table= schema_tables;
  st_add_schema_table add_data;
  DBUG_ENTER("schema_tables_add");

  for (; tmp_schema_table->table_name; tmp_schema_table++)
  {
    if (tmp_schema_table->hidden)
      continue;
    if (wild)
    {
      if (lower_case_table_names)
      {
        if (wild_case_compare(files_charset_info,
                              tmp_schema_table->table_name,
                              wild))
          continue;
      }
      else if (wild_compare(tmp_schema_table->table_name, wild, 0))
        continue;
    }
    if (files->push_back(thd->strdup(tmp_schema_table->table_name)))
      DBUG_RETURN(1);
  }

  add_data.files= files;
  add_data.wild= wild;
  if (plugin_foreach(thd, add_schema_table,
                     MYSQL_INFORMATION_SCHEMA_PLUGIN, &add_data))
      DBUG_RETURN(1);

  DBUG_RETURN(0);
}


int get_all_tables(THD *thd, TABLE_LIST *tables, COND *cond)
{
  LEX *lex= thd->lex;
  TABLE *table= tables->table;
  SELECT_LEX *select_lex= &lex->select_lex;
  SELECT_LEX *old_all_select_lex= lex->all_selects_list;
  enum_sql_command save_sql_command= lex->sql_command;
  SELECT_LEX *lsel= tables->schema_select_lex;
  ST_SCHEMA_TABLE *schema_table= tables->schema_table;
  SELECT_LEX sel;
  INDEX_FIELD_VALUES idx_field_vals;
  char path[FN_REFLEN], *base_name, *orig_base_name, *file_name;
  uint len;
  bool with_i_schema;
  enum enum_schema_tables schema_table_idx;
  List<char> bases;
  List_iterator_fast<char> it(bases);
  COND *partial_cond;
  uint derived_tables= lex->derived_tables; 
  int error= 1;
  enum legacy_db_type not_used;
  Open_tables_state open_tables_state_backup;
  bool save_view_prepare_mode= lex->view_prepare_mode;
  Query_tables_list query_tables_list_backup;
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  Security_context *sctx= thd->security_ctx;
#endif
  DBUG_ENTER("get_all_tables");

  LINT_INIT(len);

  lex->view_prepare_mode= TRUE;
  lex->reset_n_backup_query_tables_list(&query_tables_list_backup);

  /*
    We should not introduce deadlocks even if we already have some
    tables open and locked, since we won't lock tables which we will
    open and will ignore possible name-locks for these tables.
  */
  thd->reset_n_backup_open_tables_state(&open_tables_state_backup);

  if (lsel && lsel->table_list.first)
  {
    TABLE_LIST *show_table_list= (TABLE_LIST*) lsel->table_list.first;
    bool res;

    lex->all_selects_list= lsel;
    /*
      Restore thd->temporary_tables to be able to process
      temporary tables(only for 'show index' & 'show columns').
      This should be changed when processing of temporary tables for
      I_S tables will be done.
    */
    thd->temporary_tables= open_tables_state_backup.temporary_tables;
    /*
      Let us set fake sql_command so views won't try to merge
      themselves into main statement. If we don't do this,
      SELECT * from information_schema.xxxx will cause problems.
      SQLCOM_SHOW_FIELDS is used because it satisfies 'only_view_structure()' 
    */
    lex->sql_command= SQLCOM_SHOW_FIELDS;
    res= open_normal_and_derived_tables(thd, show_table_list,
                                        MYSQL_LOCK_IGNORE_FLUSH);
    lex->sql_command= save_sql_command;
    /*
      get_all_tables() returns 1 on failure and 0 on success thus
      return only these and not the result code of ::process_table()

      We should use show_table_list->alias instead of 
      show_table_list->table_name because table_name
      could be changed during opening of I_S tables. It's safe
      to use alias because alias contains original table name 
      in this case(this part of code is used only for 
      'show columns' & 'show statistics' commands).
    */
    error= test(schema_table->process_table(thd, show_table_list,
                                            table, res, 
                                            (show_table_list->view ?
                                             show_table_list->view_db.str :
                                             show_table_list->db),
                                            show_table_list->alias));
    thd->temporary_tables= 0;
    close_tables_for_reopen(thd, &show_table_list);
    goto err;
  }

  schema_table_idx= get_schema_table_idx(schema_table);

  if (make_db_list(thd, &bases, &idx_field_vals,
                   &with_i_schema, 0))
    goto err;

  partial_cond= make_cond_for_info_schema(cond, tables);
  it.rewind(); /* To get access to new elements in basis list */

  /*
    Below we generate error for non existing database.
    (to save old behaviour for SHOW TABLES FROM db)
  */
  while ((orig_base_name= base_name= it++) ||
         ((sql_command_flags[save_sql_command] & CF_SHOW_TABLE_COMMAND) &&
	  (base_name= select_lex->db) && !bases.elements))
  {
#ifndef NO_EMBEDDED_ACCESS_CHECKS
    if (!check_access(thd,SELECT_ACL, base_name, 
                      &thd->col_access, 0, 1, with_i_schema) ||
        sctx->master_access & (DB_ACLS | SHOW_DB_ACL) ||
	acl_get(sctx->host, sctx->ip, sctx->priv_user, base_name,0) ||
	!check_grant_db(thd, base_name))
#endif
    {
      List<char> files;
      if (with_i_schema)                      // information schema table names
      {
        if (schema_tables_add(thd, &files, idx_field_vals.table_value))
          goto err;
      }
      else
      {
        len= build_table_filename(path, sizeof(path), base_name, "", "", 0);
        len= FN_LEN - len;
        find_files_result res= find_files(thd, &files, base_name, 
                                          path, idx_field_vals.table_value, 0);
        if (res != FIND_FILES_OK)
        {
          /*
            Downgrade errors about problems with database directory to
            warnings if this is not a 'SHOW' command.  Another thread
            may have dropped database, and we may still have a name
            for that directory.
          */
          if (res == FIND_FILES_DIR && lex->sql_command == SQLCOM_END)
          {
            push_warning(thd, MYSQL_ERROR::WARN_LEVEL_WARN,
                         thd->net.last_errno, thd->net.last_error);
            thd->clear_error();
            continue;
          }
          else
          {
            goto err;
          }
        }
        if (lower_case_table_names)
          orig_base_name= thd->strdup(base_name);
      }

      List_iterator_fast<char> it_files(files);
      while ((file_name= it_files++))
      {
	restore_record(table, s->default_values);
        table->field[schema_table->idx_field1]->
          store(base_name, strlen(base_name), system_charset_info);
        table->field[schema_table->idx_field2]->
          store(file_name, strlen(file_name),system_charset_info);
        if (!partial_cond || partial_cond->val_int())
        {
          if (schema_table_idx == SCH_TABLE_NAMES)
          {
            if (lex->verbose ||
                (sql_command_flags[save_sql_command] & CF_STATUS_COMMAND) == 0)
            {
              if (with_i_schema)
              {
                table->field[3]->store(STRING_WITH_LEN("SYSTEM VIEW"),
                                       system_charset_info);
              }
              else
              {
                build_table_filename(path, sizeof(path),
                                     base_name, file_name, reg_ext, 0);

                switch (mysql_frm_type(thd, path, &not_used)) {
                case FRMTYPE_ERROR:
                  table->field[3]->store(STRING_WITH_LEN("ERROR"),
                                         system_charset_info);
                  break;
                case FRMTYPE_TABLE:
                  table->field[3]->store(STRING_WITH_LEN("BASE TABLE"),
                                         system_charset_info);
                  break;
                case FRMTYPE_VIEW:
                  table->field[3]->store(STRING_WITH_LEN("VIEW"),
                                         system_charset_info);
                  break;
                default:
                  DBUG_ASSERT(0);
                }
              }
            }
            if (schema_table_store_record(thd, table))
              goto err;
          }
          else
          {
            int res;
            /*
              Set the parent lex of 'sel' because it is needed by
              sel.init_query() which is called inside make_table_list.
            */
            sel.parent_lex= lex;
            if (make_table_list(thd, &sel, base_name, file_name))
              goto err;
            TABLE_LIST *show_table_list= (TABLE_LIST*) sel.table_list.first;
            lex->all_selects_list= &sel;
            lex->derived_tables= 0;
            lex->sql_command= SQLCOM_SHOW_FIELDS;
            res= open_normal_and_derived_tables(thd, show_table_list,
                                                MYSQL_LOCK_IGNORE_FLUSH);
            lex->sql_command= save_sql_command;
            /* 
              They can drop table after table names list creation and
              before table opening. We open non existing table and 
              get ER_NO_SUCH_TABLE error. In this case we do not store
              the record into I_S table and clear error.
            */
            if (thd->net.last_errno == ER_NO_SUCH_TABLE)
            {
              res= 0;
              thd->clear_error();
            }
            else
            {
              /*
                We should use show_table_list->alias instead of 
                show_table_list->table_name because table_name
                could be changed during opening of I_S tables. It's safe
                to use alias because alias contains original table name 
                in this case.
              */
              res= schema_table->process_table(thd, show_table_list, table,
                                               res, orig_base_name,
                                               show_table_list->alias);
              close_tables_for_reopen(thd, &show_table_list);
              DBUG_ASSERT(!lex->query_tables_own_last);
            }
            if (res)
              goto err;
          }
        }
      }
      /*
        If we have information schema its always the first table and only
        the first table. Reset for other tables.
      */
      with_i_schema= 0;
    }
  }

  error= 0;
err:
  thd->restore_backup_open_tables_state(&open_tables_state_backup);
  lex->restore_backup_query_tables_list(&query_tables_list_backup);
  lex->derived_tables= derived_tables;
  lex->all_selects_list= old_all_select_lex;
  lex->view_prepare_mode= save_view_prepare_mode;
  lex->sql_command= save_sql_command;
  DBUG_RETURN(error);
}


bool store_schema_shemata(THD* thd, TABLE *table, const char *db_name,
                          CHARSET_INFO *cs)
{
  restore_record(table, s->default_values);
  table->field[1]->store(db_name, strlen(db_name), system_charset_info);
  table->field[2]->store(cs->csname, strlen(cs->csname), system_charset_info);
  table->field[3]->store(cs->name, strlen(cs->name), system_charset_info);
  return schema_table_store_record(thd, table);
}


int fill_schema_shemata(THD *thd, TABLE_LIST *tables, COND *cond)
{
  /*
    TODO: fill_schema_shemata() is called when new client is connected.
    Returning error status in this case leads to client hangup.
  */

  INDEX_FIELD_VALUES idx_field_vals;
  List<char> files;
  char *file_name;
  bool with_i_schema;
  HA_CREATE_INFO create;
  TABLE *table= tables->table;
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  Security_context *sctx= thd->security_ctx;
#endif
  DBUG_ENTER("fill_schema_shemata");

  if (make_db_list(thd, &files, &idx_field_vals,
                   &with_i_schema, 1))
    DBUG_RETURN(1);

  List_iterator_fast<char> it(files);
  while ((file_name=it++))
  {
    if (with_i_schema)       // information schema name is always first in list
    {
      if (store_schema_shemata(thd, table, file_name,
                               system_charset_info))
        DBUG_RETURN(1);
      with_i_schema= 0;
      continue;
    }
#ifndef NO_EMBEDDED_ACCESS_CHECKS
    if (sctx->master_access & (DB_ACLS | SHOW_DB_ACL) ||
	acl_get(sctx->host, sctx->ip, sctx->priv_user, file_name,0) ||
	!check_grant_db(thd, file_name))
#endif
    {
      load_db_opt_by_name(thd, file_name, &create);

      if (store_schema_shemata(thd, table, file_name,
                               create.default_table_charset))
        DBUG_RETURN(1);
    }
  }
  DBUG_RETURN(0);
}


static int get_schema_tables_record(THD *thd, struct st_table_list *tables,
				    TABLE *table, bool res,
				    const char *base_name,
				    const char *file_name)
{
  const char *tmp_buff;
  MYSQL_TIME time;
  CHARSET_INFO *cs= system_charset_info;
  DBUG_ENTER("get_schema_tables_record");

  restore_record(table, s->default_values);
  table->field[1]->store(base_name, strlen(base_name), cs);
  table->field[2]->store(file_name, strlen(file_name), cs);
  if (res)
  {
    /*
      there was errors during opening tables
    */
    const char *error= thd->net.last_error;
    if (tables->view)
      table->field[3]->store(STRING_WITH_LEN("VIEW"), cs);
    else if (tables->schema_table)
      table->field[3]->store(STRING_WITH_LEN("SYSTEM VIEW"), cs);
    else
      table->field[3]->store(STRING_WITH_LEN("BASE TABLE"), cs);
    table->field[20]->store(error, strlen(error), cs);
    thd->clear_error();
  }
  else if (tables->view)
  {
    table->field[3]->store(STRING_WITH_LEN("VIEW"), cs);
    table->field[20]->store(STRING_WITH_LEN("VIEW"), cs);
  }
  else
  {
    TABLE *show_table= tables->table;
    TABLE_SHARE *share= show_table->s;
    handler *file= show_table->file;

    file->info(HA_STATUS_VARIABLE | HA_STATUS_TIME | HA_STATUS_AUTO |
               HA_STATUS_NO_LOCK);
    if (share->tmp_table == SYSTEM_TMP_TABLE)
      table->field[3]->store(STRING_WITH_LEN("SYSTEM VIEW"), cs);
    else if (share->tmp_table)
      table->field[3]->store(STRING_WITH_LEN("LOCAL TEMPORARY"), cs);
    else
      table->field[3]->store(STRING_WITH_LEN("BASE TABLE"), cs);

    for (int i= 4; i < 20; i++)
    {
      if (i == 7 || (i > 12 && i < 17) || i == 18)
        continue;
      table->field[i]->set_notnull();
    }
    tmp_buff= file->table_type();
    table->field[4]->store(tmp_buff, strlen(tmp_buff), cs);
    table->field[5]->store((longlong) share->frm_version, TRUE);
    enum row_type row_type = file->get_row_type();
    switch (row_type) {
    case ROW_TYPE_NOT_USED:
    case ROW_TYPE_DEFAULT:
      tmp_buff= ((share->db_options_in_use &
		  HA_OPTION_COMPRESS_RECORD) ? "Compressed" :
		 (share->db_options_in_use & HA_OPTION_PACK_RECORD) ?
		 "Dynamic" : "Fixed");
      break;
    case ROW_TYPE_FIXED:
      tmp_buff= "Fixed";
      break;
    case ROW_TYPE_DYNAMIC:
      tmp_buff= "Dynamic";
      break;
    case ROW_TYPE_COMPRESSED:
      tmp_buff= "Compressed";
      break;
    case ROW_TYPE_REDUNDANT:
      tmp_buff= "Redundant";
      break;
    case ROW_TYPE_COMPACT:
      tmp_buff= "Compact";
      break;
    case ROW_TYPE_PAGE:
      tmp_buff= "Page";
      break;
    }
    table->field[6]->store(tmp_buff, strlen(tmp_buff), cs);
    if (!tables->schema_table)
    {
      table->field[7]->store((longlong) file->stats.records, TRUE);
      table->field[7]->set_notnull();
    }
    table->field[8]->store((longlong) file->stats.mean_rec_length, TRUE);
    table->field[9]->store((longlong) file->stats.data_file_length, TRUE);
    if (file->stats.max_data_file_length)
    {
      table->field[10]->store((longlong) file->stats.max_data_file_length,
                              TRUE);
    }
    table->field[11]->store((longlong) file->stats.index_file_length, TRUE);
    table->field[12]->store((longlong) file->stats.delete_length, TRUE);
    if (show_table->found_next_number_field)
    {
      table->field[13]->store((longlong) file->stats.auto_increment_value,
                              TRUE);
      table->field[13]->set_notnull();
    }
    if (file->stats.create_time)
    {
      thd->variables.time_zone->gmt_sec_to_TIME(&time,
                                                (my_time_t) file->stats.create_time);
      table->field[14]->store_time(&time, MYSQL_TIMESTAMP_DATETIME);
      table->field[14]->set_notnull();
    }
    if (file->stats.update_time)
    {
      thd->variables.time_zone->gmt_sec_to_TIME(&time,
                                                (my_time_t) file->stats.update_time);
      table->field[15]->store_time(&time, MYSQL_TIMESTAMP_DATETIME);
      table->field[15]->set_notnull();
    }
    if (file->stats.check_time)
    {
      thd->variables.time_zone->gmt_sec_to_TIME(&time,
                                                (my_time_t) file->stats.check_time);
      table->field[16]->store_time(&time, MYSQL_TIMESTAMP_DATETIME);
      table->field[16]->set_notnull();
    }
    tmp_buff= (share->table_charset ?
               share->table_charset->name : "default");
    table->field[17]->store(tmp_buff, strlen(tmp_buff), cs);
    if (file->ha_table_flags() & (ulong) HA_HAS_CHECKSUM)
    {
      table->field[18]->store((longlong) file->checksum(), TRUE);
      table->field[18]->set_notnull();
    }

    char option_buff[350],*ptr;
    ptr=option_buff;
    if (share->min_rows)
    {
      ptr=strmov(ptr," min_rows=");
      ptr=longlong10_to_str(share->min_rows,ptr,10);
    }
    if (share->max_rows)
    {
      ptr=strmov(ptr," max_rows=");
      ptr=longlong10_to_str(share->max_rows,ptr,10);
    }
    if (share->avg_row_length)
    {
      ptr=strmov(ptr," avg_row_length=");
      ptr=longlong10_to_str(share->avg_row_length,ptr,10);
    }
    if (share->db_create_options & HA_OPTION_PACK_KEYS)
      ptr=strmov(ptr," pack_keys=1");
    if (share->db_create_options & HA_OPTION_NO_PACK_KEYS)
      ptr=strmov(ptr," pack_keys=0");
    if (share->db_create_options & HA_OPTION_CHECKSUM)
      ptr=strmov(ptr," checksum=1");
    if (share->db_create_options & HA_OPTION_DELAY_KEY_WRITE)
      ptr=strmov(ptr," delay_key_write=1");
    if (share->row_type != ROW_TYPE_DEFAULT)
      ptr=strxmov(ptr, " row_format=", 
                  ha_row_type[(uint) share->row_type],
                  NullS);
#ifdef WITH_PARTITION_STORAGE_ENGINE
    if (show_table->s->db_type() == partition_hton && 
        show_table->part_info != NULL && 
        show_table->part_info->no_parts > 0)
      ptr= strmov(ptr, " partitioned");
#endif
    table->field[19]->store(option_buff+1,
                            (ptr == option_buff ? 0 : 
                             (uint) (ptr-option_buff)-1), cs);
    {
      char *comment;
      comment= show_table->file->update_table_comment(share->comment.str);
      if (comment)
      {
        table->field[20]->store(comment,
                                (comment == share->comment.str ?
                                 share->comment.length : 
                                 strlen(comment)), cs);
        if (comment != share->comment.str)
          my_free(comment, MYF(0));
      }
    }
  }
  DBUG_RETURN(schema_table_store_record(thd, table));
}


static int get_schema_column_record(THD *thd, struct st_table_list *tables,
				    TABLE *table, bool res,
				    const char *base_name,
				    const char *file_name)
{
  LEX *lex= thd->lex;
  const char *wild= lex->wild ? lex->wild->ptr() : NullS;
  CHARSET_INFO *cs= system_charset_info;
  TABLE *show_table;
  handler *file;
  Field **ptr,*field;
  int count;
  uint base_name_length, file_name_length;
  DBUG_ENTER("get_schema_column_record");

  if (res)
  {
    if (lex->sql_command != SQLCOM_SHOW_FIELDS)
    {
      /*
        I.e. we are in SELECT FROM INFORMATION_SCHEMA.COLUMS
        rather than in SHOW COLUMNS
      */ 
      push_warning(thd, MYSQL_ERROR::WARN_LEVEL_WARN,
                   thd->net.last_errno, thd->net.last_error);
      thd->clear_error();
      res= 0;
    }
    DBUG_RETURN(res);
  }

  show_table= tables->table;
  file= show_table->file;
  count= 0;
  file->info(HA_STATUS_VARIABLE | HA_STATUS_NO_LOCK);
  restore_record(show_table, s->default_values);
  base_name_length= strlen(base_name);
  file_name_length= strlen(file_name);
  show_table->use_all_columns();               // Required for default

  for (ptr=show_table->field; (field= *ptr) ; ptr++)
  {
    const char *tmp_buff;
    uchar *pos;
    bool is_blob;
    uint flags=field->flags;
    char tmp[MAX_FIELD_WIDTH];
    char tmp1[MAX_FIELD_WIDTH];
    String type(tmp,sizeof(tmp), system_charset_info);
    char *end;
    int decimals, field_length;

    if (wild && wild[0] &&
        wild_case_compare(system_charset_info, field->field_name,wild))
      continue;

    flags= field->flags;
    count++;
    /* Get default row, with all NULL fields set to NULL */
    restore_record(table, s->default_values);

#ifndef NO_EMBEDDED_ACCESS_CHECKS
    uint col_access;
    check_access(thd,SELECT_ACL | EXTRA_ACL, base_name,
                 &tables->grant.privilege, 0, 0, test(tables->schema_table));
    col_access= get_column_grant(thd, &tables->grant, 
                                 base_name, file_name,
                                 field->field_name) & COL_ACLS;
    if (lex->sql_command != SQLCOM_SHOW_FIELDS  &&
        !tables->schema_table && !col_access)
      continue;
    end= tmp;
    for (uint bitnr=0; col_access ; col_access>>=1,bitnr++)
    {
      if (col_access & 1)
      {
        *end++=',';
        end=strmov(end,grant_types.type_names[bitnr]);
      }
    }
    table->field[17]->store(tmp+1,end == tmp ? 0 : (uint) (end-tmp-1), cs);

#endif
    table->field[1]->store(base_name, base_name_length, cs);
    table->field[2]->store(file_name, file_name_length, cs);
    table->field[3]->store(field->field_name, strlen(field->field_name),
                           cs);
    table->field[4]->store((longlong) count, TRUE);
    field->sql_type(type);
    table->field[14]->store(type.ptr(), type.length(), cs);		
    tmp_buff= strchr(type.ptr(), '(');
    table->field[7]->store(type.ptr(),
                           (tmp_buff ? tmp_buff - type.ptr() :
                            type.length()), cs);
    if (show_table->timestamp_field == field &&
        field->unireg_check != Field::TIMESTAMP_UN_FIELD)
    {
      table->field[5]->store(STRING_WITH_LEN("CURRENT_TIMESTAMP"), cs);
      table->field[5]->set_notnull();
    }
    else if (field->unireg_check != Field::NEXT_NUMBER &&
             !field->is_null() &&
             !(field->flags & NO_DEFAULT_VALUE_FLAG))
    {
      String def(tmp1,sizeof(tmp1), cs);
      type.set(tmp, sizeof(tmp), field->charset());
      field->val_str(&type);
      uint dummy_errors;
      def.copy(type.ptr(), type.length(), type.charset(), cs, &dummy_errors);
      table->field[5]->store(def.ptr(), def.length(), def.charset());
      table->field[5]->set_notnull();
    }
    else if (field->unireg_check == Field::NEXT_NUMBER ||
             lex->sql_command != SQLCOM_SHOW_FIELDS ||
             field->maybe_null())
      table->field[5]->set_null();                // Null as default
    else
    {
      table->field[5]->store("",0, cs);
      table->field[5]->set_notnull();
    }
    pos=(uchar*) ((flags & NOT_NULL_FLAG) ?  "NO" : "YES");
    table->field[6]->store((const char*) pos,
                           strlen((const char*) pos), cs);
    is_blob= (field->type() == MYSQL_TYPE_BLOB);
    if (field->has_charset() || is_blob ||
        field->real_type() == MYSQL_TYPE_VARCHAR ||  // For varbinary type
        field->real_type() == MYSQL_TYPE_STRING)     // For binary type
    {
      uint32 octet_max_length= field->max_display_length();
      if (is_blob && octet_max_length != (uint32) 4294967295U)
        octet_max_length /= field->charset()->mbmaxlen;
      longlong char_max_len= is_blob ? 
        (longlong) octet_max_length / field->charset()->mbminlen :
        (longlong) octet_max_length / field->charset()->mbmaxlen;
      table->field[8]->store(char_max_len, TRUE);
      table->field[8]->set_notnull();
      table->field[9]->store((longlong) octet_max_length, TRUE);
      table->field[9]->set_notnull();
    }

    /*
      Calculate field_length and decimals.
      They are set to -1 if they should not be set (we should return NULL)
    */

    decimals= field->decimals();
    switch (field->type()) {
    case MYSQL_TYPE_NEWDECIMAL:
      field_length= ((Field_new_decimal*) field)->precision;
      break;
    case MYSQL_TYPE_DECIMAL:
      field_length= field->field_length - (decimals  ? 2 : 1);
      break;
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG:
    case MYSQL_TYPE_INT24:
      field_length= field->max_display_length() - 1;
      break;
    case MYSQL_TYPE_BIT:
      field_length= field->max_display_length();
      decimals= -1;                             // return NULL
      break;
    case MYSQL_TYPE_FLOAT:  
    case MYSQL_TYPE_DOUBLE:
      field_length= field->field_length;
      if (decimals == NOT_FIXED_DEC)
        decimals= -1;                           // return NULL
    break;
    default:
      field_length= decimals= -1;
      break;
    }

    if (field_length >= 0)
    {
      table->field[10]->store((longlong) field_length, TRUE);
      table->field[10]->set_notnull();
    }
    if (decimals >= 0)
    {
      table->field[11]->store((longlong) decimals, TRUE);
      table->field[11]->set_notnull();
    }

    if (field->has_charset())
    {
      pos=(uchar*) field->charset()->csname;
      table->field[12]->store((const char*) pos,
                              strlen((const char*) pos), cs);
      table->field[12]->set_notnull();
      pos=(uchar*) field->charset()->name;
      table->field[13]->store((const char*) pos,
                              strlen((const char*) pos), cs);
      table->field[13]->set_notnull();
    }
    pos=(uchar*) ((field->flags & PRI_KEY_FLAG) ? "PRI" :
                 (field->flags & UNIQUE_KEY_FLAG) ? "UNI" :
                 (field->flags & MULTIPLE_KEY_FLAG) ? "MUL":"");
    table->field[15]->store((const char*) pos,
                            strlen((const char*) pos), cs);

    end= tmp;
    if (field->unireg_check == Field::NEXT_NUMBER)
      end=strmov(tmp,"auto_increment");
    table->field[16]->store(tmp, (uint) (end-tmp), cs);

    table->field[18]->store(field->comment.str, field->comment.length, cs);
    if (schema_table_store_record(thd, table))
      DBUG_RETURN(1);
  }
  DBUG_RETURN(0);
}



int fill_schema_charsets(THD *thd, TABLE_LIST *tables, COND *cond)
{
  CHARSET_INFO **cs;
  const char *wild= thd->lex->wild ? thd->lex->wild->ptr() : NullS;
  TABLE *table= tables->table;
  CHARSET_INFO *scs= system_charset_info;

  for (cs= all_charsets ; cs < all_charsets+255 ; cs++)
  {
    CHARSET_INFO *tmp_cs= cs[0];
    if (tmp_cs && (tmp_cs->state & MY_CS_PRIMARY) && 
        (tmp_cs->state & MY_CS_AVAILABLE) &&
        !(tmp_cs->state & MY_CS_HIDDEN) &&
        !(wild && wild[0] &&
	  wild_case_compare(scs, tmp_cs->csname,wild)))
    {
      const char *comment;
      restore_record(table, s->default_values);
      table->field[0]->store(tmp_cs->csname, strlen(tmp_cs->csname), scs);
      table->field[1]->store(tmp_cs->name, strlen(tmp_cs->name), scs);
      comment= tmp_cs->comment ? tmp_cs->comment : "";
      table->field[2]->store(comment, strlen(comment), scs);
      table->field[3]->store((longlong) tmp_cs->mbmaxlen, TRUE);
      if (schema_table_store_record(thd, table))
        return 1;
    }
  }
  return 0;
}


static my_bool iter_schema_engines(THD *thd, plugin_ref plugin,
                                   void *ptable)
{
  TABLE *table= (TABLE *) ptable;
  handlerton *hton= plugin_data(plugin, handlerton *);
  const char *wild= thd->lex->wild ? thd->lex->wild->ptr() : NullS;
  CHARSET_INFO *scs= system_charset_info;
  handlerton *default_type= ha_default_handlerton(thd);
  DBUG_ENTER("iter_schema_engines");

  if (!(hton->flags & HTON_HIDDEN))
  {
    LEX_STRING *name= plugin_name(plugin);
    if (!(wild && wild[0] &&
          wild_case_compare(scs, name->str,wild)))
    {
      LEX_STRING yesno[2]= {{ C_STRING_WITH_LEN("NO") },
                            { C_STRING_WITH_LEN("YES") }};
      LEX_STRING *tmp;
      const char *option_name= show_comp_option_name[(int) hton->state];
      restore_record(table, s->default_values);

      table->field[0]->store(name->str, name->length, scs);
      if (hton->state == SHOW_OPTION_YES && default_type == hton)
        option_name= "DEFAULT";
      table->field[1]->store(option_name, strlen(option_name), scs);
      table->field[2]->store(plugin_decl(plugin)->descr,
                             strlen(plugin_decl(plugin)->descr), scs);
      tmp= &yesno[test(hton->commit)];
      table->field[3]->store(tmp->str, tmp->length, scs);
      tmp= &yesno[test(hton->prepare)];
      table->field[4]->store(tmp->str, tmp->length, scs);
      tmp= &yesno[test(hton->savepoint_set)];
      table->field[5]->store(tmp->str, tmp->length, scs);

      if (schema_table_store_record(thd, table))
        DBUG_RETURN(1);
    }
  }
  DBUG_RETURN(0);
}


int fill_schema_engines(THD *thd, TABLE_LIST *tables, COND *cond)
{
  return plugin_foreach(thd, iter_schema_engines,
                        MYSQL_STORAGE_ENGINE_PLUGIN, tables->table);
}


int fill_schema_collation(THD *thd, TABLE_LIST *tables, COND *cond)
{
  CHARSET_INFO **cs;
  const char *wild= thd->lex->wild ? thd->lex->wild->ptr() : NullS;
  TABLE *table= tables->table;
  CHARSET_INFO *scs= system_charset_info;
  for (cs= all_charsets ; cs < all_charsets+255 ; cs++ )
  {
    CHARSET_INFO **cl;
    CHARSET_INFO *tmp_cs= cs[0];
    if (!tmp_cs || !(tmp_cs->state & MY_CS_AVAILABLE) ||
         (tmp_cs->state & MY_CS_HIDDEN) ||
        !(tmp_cs->state & MY_CS_PRIMARY))
      continue;
    for (cl= all_charsets; cl < all_charsets+255 ;cl ++)
    {
      CHARSET_INFO *tmp_cl= cl[0];
      if (!tmp_cl || !(tmp_cl->state & MY_CS_AVAILABLE) || 
          !my_charset_same(tmp_cs, tmp_cl))
	continue;
      if (!(wild && wild[0] &&
	  wild_case_compare(scs, tmp_cl->name,wild)))
      {
	const char *tmp_buff;
	restore_record(table, s->default_values);
	table->field[0]->store(tmp_cl->name, strlen(tmp_cl->name), scs);
        table->field[1]->store(tmp_cl->csname , strlen(tmp_cl->csname), scs);
        table->field[2]->store((longlong) tmp_cl->number, TRUE);
        tmp_buff= (tmp_cl->state & MY_CS_PRIMARY) ? "Yes" : "";
	table->field[3]->store(tmp_buff, strlen(tmp_buff), scs);
        tmp_buff= (tmp_cl->state & MY_CS_COMPILED)? "Yes" : "";
	table->field[4]->store(tmp_buff, strlen(tmp_buff), scs);
        table->field[5]->store((longlong) tmp_cl->strxfrm_multiply, TRUE);
        if (schema_table_store_record(thd, table))
          return 1;
      }
    }
  }
  return 0;
}


int fill_schema_coll_charset_app(THD *thd, TABLE_LIST *tables, COND *cond)
{
  CHARSET_INFO **cs;
  TABLE *table= tables->table;
  CHARSET_INFO *scs= system_charset_info;
  for (cs= all_charsets ; cs < all_charsets+255 ; cs++ )
  {
    CHARSET_INFO **cl;
    CHARSET_INFO *tmp_cs= cs[0];
    if (!tmp_cs || !(tmp_cs->state & MY_CS_AVAILABLE) || 
        !(tmp_cs->state & MY_CS_PRIMARY))
      continue;
    for (cl= all_charsets; cl < all_charsets+255 ;cl ++)
    {
      CHARSET_INFO *tmp_cl= cl[0];
      if (!tmp_cl || !(tmp_cl->state & MY_CS_AVAILABLE) || 
          !my_charset_same(tmp_cs,tmp_cl))
	continue;
      restore_record(table, s->default_values);
      table->field[0]->store(tmp_cl->name, strlen(tmp_cl->name), scs);
      table->field[1]->store(tmp_cl->csname , strlen(tmp_cl->csname), scs);
      if (schema_table_store_record(thd, table))
        return 1;
    }
  }
  return 0;
}


bool store_schema_proc(THD *thd, TABLE *table, TABLE *proc_table,
                       const char *wild, bool full_access, const char *sp_user)
{
  String tmp_string;
  String sp_db, sp_name, definer;
  MYSQL_TIME time;
  LEX *lex= thd->lex;
  CHARSET_INFO *cs= system_charset_info;
  get_field(thd->mem_root, proc_table->field[0], &sp_db);
  get_field(thd->mem_root, proc_table->field[1], &sp_name);
  get_field(thd->mem_root, proc_table->field[11], &definer);
  if (!full_access)
    full_access= !strcmp(sp_user, definer.ptr());
  if (!full_access && check_some_routine_access(thd, sp_db.ptr(),
                                                sp_name.ptr(),
                                                proc_table->field[2]->
                                                val_int() ==
                                                TYPE_ENUM_PROCEDURE))
    return 0;

  if (lex->sql_command == SQLCOM_SHOW_STATUS_PROC &&
      proc_table->field[2]->val_int() == TYPE_ENUM_PROCEDURE ||
      lex->sql_command == SQLCOM_SHOW_STATUS_FUNC &&
      proc_table->field[2]->val_int() == TYPE_ENUM_FUNCTION ||
      (sql_command_flags[lex->sql_command] & CF_STATUS_COMMAND) == 0)
  {
    restore_record(table, s->default_values);
    if (!wild || !wild[0] || !wild_compare(sp_name.ptr(), wild, 0))
    {
      int enum_idx= (int) proc_table->field[5]->val_int();
      table->field[3]->store(sp_name.ptr(), sp_name.length(), cs);
      get_field(thd->mem_root, proc_table->field[3], &tmp_string);
      table->field[0]->store(tmp_string.ptr(), tmp_string.length(), cs);
      table->field[2]->store(sp_db.ptr(), sp_db.length(), cs);
      get_field(thd->mem_root, proc_table->field[2], &tmp_string);
      table->field[4]->store(tmp_string.ptr(), tmp_string.length(), cs);
      if (proc_table->field[2]->val_int() == TYPE_ENUM_FUNCTION)
      {
        get_field(thd->mem_root, proc_table->field[9], &tmp_string);
        table->field[5]->store(tmp_string.ptr(), tmp_string.length(), cs);
        table->field[5]->set_notnull();
      }
      if (full_access)
      {
        get_field(thd->mem_root, proc_table->field[19], &tmp_string);
        table->field[7]->store(tmp_string.ptr(), tmp_string.length(), cs);
        table->field[7]->set_notnull();
      }
      table->field[6]->store(STRING_WITH_LEN("SQL"), cs);
      table->field[10]->store(STRING_WITH_LEN("SQL"), cs);
      get_field(thd->mem_root, proc_table->field[6], &tmp_string);
      table->field[11]->store(tmp_string.ptr(), tmp_string.length(), cs);
      table->field[12]->store(sp_data_access_name[enum_idx].str, 
                              sp_data_access_name[enum_idx].length , cs);
      get_field(thd->mem_root, proc_table->field[7], &tmp_string);
      table->field[14]->store(tmp_string.ptr(), tmp_string.length(), cs);
      bzero((char *)&time, sizeof(time));
      ((Field_timestamp *) proc_table->field[12])->get_time(&time);
      table->field[15]->store_time(&time, MYSQL_TIMESTAMP_DATETIME);
      bzero((char *)&time, sizeof(time));
      ((Field_timestamp *) proc_table->field[13])->get_time(&time);
      table->field[16]->store_time(&time, MYSQL_TIMESTAMP_DATETIME);
      get_field(thd->mem_root, proc_table->field[14], &tmp_string);
      table->field[17]->store(tmp_string.ptr(), tmp_string.length(), cs);
      get_field(thd->mem_root, proc_table->field[15], &tmp_string);
      table->field[18]->store(tmp_string.ptr(), tmp_string.length(), cs);
      table->field[19]->store(definer.ptr(), definer.length(), cs);

      get_field(thd->mem_root, proc_table->field[16], &tmp_string);
      table->field[20]->store(tmp_string.ptr(), tmp_string.length(), cs);

      get_field(thd->mem_root, proc_table->field[17], &tmp_string);
      table->field[21]->store(tmp_string.ptr(), tmp_string.length(), cs);

      get_field(thd->mem_root, proc_table->field[18], &tmp_string);
      table->field[22]->store(tmp_string.ptr(), tmp_string.length(), cs);

      return schema_table_store_record(thd, table);
    }
  }
  return 0;
}


int fill_schema_proc(THD *thd, TABLE_LIST *tables, COND *cond)
{
  TABLE *proc_table;
  TABLE_LIST proc_tables;
  const char *wild= thd->lex->wild ? thd->lex->wild->ptr() : NullS;
  int res= 0;
  TABLE *table= tables->table;
  bool full_access;
  char definer[USER_HOST_BUFF_SIZE];
  Open_tables_state open_tables_state_backup;
  DBUG_ENTER("fill_schema_proc");

  strxmov(definer, thd->security_ctx->priv_user, "@",
          thd->security_ctx->priv_host, NullS);
  /* We use this TABLE_LIST instance only for checking of privileges. */
  bzero((char*) &proc_tables,sizeof(proc_tables));
  proc_tables.db= (char*) "mysql";
  proc_tables.db_length= 5;
  proc_tables.table_name= proc_tables.alias= (char*) "proc";
  proc_tables.table_name_length= 4;
  proc_tables.lock_type= TL_READ;
  full_access= !check_table_access(thd, SELECT_ACL, &proc_tables, 1);
  if (!(proc_table= open_proc_table_for_read(thd, &open_tables_state_backup)))
  {
    DBUG_RETURN(1);
  }
  proc_table->file->ha_index_init(0, 1);
  if ((res= proc_table->file->index_first(proc_table->record[0])))
  {
    res= (res == HA_ERR_END_OF_FILE) ? 0 : 1;
    goto err;
  }
  if (store_schema_proc(thd, table, proc_table, wild, full_access, definer))
  {
    res= 1;
    goto err;
  }
  while (!proc_table->file->index_next(proc_table->record[0]))
  {
    if (store_schema_proc(thd, table, proc_table, wild, full_access, definer))
    {
      res= 1;
      goto err;
    }
  }

err:
  proc_table->file->ha_index_end();
  close_system_tables(thd, &open_tables_state_backup);
  DBUG_RETURN(res);
}


static int get_schema_stat_record(THD *thd, struct st_table_list *tables,
				  TABLE *table, bool res,
				  const char *base_name,
				  const char *file_name)
{
  CHARSET_INFO *cs= system_charset_info;
  DBUG_ENTER("get_schema_stat_record");
  if (res)
  {
    if (thd->lex->sql_command != SQLCOM_SHOW_KEYS)
    {
      /*
        I.e. we are in SELECT FROM INFORMATION_SCHEMA.STATISTICS
        rather than in SHOW KEYS
      */
      if (!tables->view)
        push_warning(thd, MYSQL_ERROR::WARN_LEVEL_WARN,
                     thd->net.last_errno, thd->net.last_error);
      thd->clear_error();
      res= 0;
    }
    DBUG_RETURN(res);
  }
  else if (!tables->view)
  {
    TABLE *show_table= tables->table;
    KEY *key_info=show_table->key_info;
    show_table->file->info(HA_STATUS_VARIABLE |
                           HA_STATUS_NO_LOCK |
                           HA_STATUS_TIME);
    for (uint i=0 ; i < show_table->s->keys ; i++,key_info++)
    {
      KEY_PART_INFO *key_part= key_info->key_part;
      const char *str;
      for (uint j=0 ; j < key_info->key_parts ; j++,key_part++)
      {
        restore_record(table, s->default_values);
        table->field[1]->store(base_name, strlen(base_name), cs);
        table->field[2]->store(file_name, strlen(file_name), cs);
        table->field[3]->store((longlong) ((key_info->flags &
                                            HA_NOSAME) ? 0 : 1), TRUE);
        table->field[4]->store(base_name, strlen(base_name), cs);
        table->field[5]->store(key_info->name, strlen(key_info->name), cs);
        table->field[6]->store((longlong) (j+1), TRUE);
        str=(key_part->field ? key_part->field->field_name :
             "?unknown field?");
        table->field[7]->store(str, strlen(str), cs);
        if (show_table->file->index_flags(i, j, 0) & HA_READ_ORDER)
        {
          table->field[8]->store(((key_part->key_part_flag &
                                   HA_REVERSE_SORT) ?
                                  "D" : "A"), 1, cs);
          table->field[8]->set_notnull();
        }
        KEY *key=show_table->key_info+i;
        if (key->rec_per_key[j])
        {
          ha_rows records=(show_table->file->stats.records /
                           key->rec_per_key[j]);
          table->field[9]->store((longlong) records, TRUE);
          table->field[9]->set_notnull();
        }
        if (!(key_info->flags & HA_FULLTEXT) &&
            (key_part->field &&
             key_part->length !=
             show_table->field[key_part->fieldnr-1]->key_length()))
        {
          table->field[10]->store((longlong) key_part->length /
                                  key_part->field->charset()->mbmaxlen, TRUE);
          table->field[10]->set_notnull();
        }
        uint flags= key_part->field ? key_part->field->flags : 0;
        const char *pos=(char*) ((flags & NOT_NULL_FLAG) ? "" : "YES");
        table->field[12]->store(pos, strlen(pos), cs);
        pos= show_table->file->index_type(i);
        table->field[13]->store(pos, strlen(pos), cs);
        if (!show_table->s->keys_in_use.is_set(i))
          table->field[14]->store(STRING_WITH_LEN("disabled"), cs);
        else
          table->field[14]->store("", 0, cs);
        table->field[14]->set_notnull();
        if (schema_table_store_record(thd, table))
          DBUG_RETURN(1);
      }
    }
  }
  DBUG_RETURN(res);
}


static int get_schema_views_record(THD *thd, struct st_table_list *tables,
				   TABLE *table, bool res,
				   const char *base_name,
				   const char *file_name)
{
  CHARSET_INFO *cs= system_charset_info;
  DBUG_ENTER("get_schema_views_record");
  char definer[USER_HOST_BUFF_SIZE];
  uint definer_len;
  bool updatable_view;

  if (tables->view)
  {
    Security_context *sctx= thd->security_ctx;
    if (!tables->allowed_show)
    {
      if (!my_strcasecmp(system_charset_info, tables->definer.user.str,
                         sctx->priv_user) &&
          !my_strcasecmp(system_charset_info, tables->definer.host.str,
                         sctx->priv_host))
        tables->allowed_show= TRUE;
    }
    restore_record(table, s->default_values);
    table->field[1]->store(tables->view_db.str, tables->view_db.length, cs);
    table->field[2]->store(tables->view_name.str, tables->view_name.length, cs);
    if (tables->allowed_show)
    {
      table->field[3]->store(tables->view_body_utf8.str,
                             tables->view_body_utf8.length,
                             cs);
    }

    if (tables->with_check != VIEW_CHECK_NONE)
    {
      if (tables->with_check == VIEW_CHECK_LOCAL)
        table->field[4]->store(STRING_WITH_LEN("LOCAL"), cs);
      else
        table->field[4]->store(STRING_WITH_LEN("CASCADED"), cs);
    }
    else
      table->field[4]->store(STRING_WITH_LEN("NONE"), cs);

    updatable_view= 0;
    if (tables->algorithm != VIEW_ALGORITHM_TMPTABLE)
    {
      /*
        We should use tables->view->select_lex.item_list here and
        can not use Field_iterator_view because the view always uses
        temporary algorithm during opening for I_S and
        TABLE_LIST fields 'field_translation' & 'field_translation_end'
        are uninitialized is this case.
      */
      List<Item> *fields= &tables->view->select_lex.item_list;
      List_iterator<Item> it(*fields);
      Item *item;
      Item_field *field;
      /*
        chech that at least one coulmn in view is updatable
      */
      while ((item= it++))
      {
        if ((field= item->filed_for_view_update()) && field->field &&
            !field->field->table->pos_in_table_list->schema_table)
        {
          updatable_view= 1;
          break;
        }
      }
    }
    if (updatable_view)
      table->field[5]->store(STRING_WITH_LEN("YES"), cs);
    else
      table->field[5]->store(STRING_WITH_LEN("NO"), cs);
    definer_len= (strxmov(definer, tables->definer.user.str, "@",
                          tables->definer.host.str, NullS) - definer);
    table->field[6]->store(definer, definer_len, cs);
    if (tables->view_suid)
      table->field[7]->store(STRING_WITH_LEN("DEFINER"), cs);
    else
      table->field[7]->store(STRING_WITH_LEN("INVOKER"), cs);

    table->field[8]->store(
      tables->view_creation_ctx->get_client_cs()->csname,
      strlen(tables->view_creation_ctx->get_client_cs()->csname),
      cs);

    table->field[9]->store(
      tables->view_creation_ctx->get_connection_cl()->name,
      strlen(tables->view_creation_ctx->get_connection_cl()->name),
      cs);

    if (schema_table_store_record(thd, table))
      DBUG_RETURN(1);
    if (res)
      push_warning(thd, MYSQL_ERROR::WARN_LEVEL_WARN, 
                   thd->net.last_errno, thd->net.last_error);
  }
  if (res) 
    thd->clear_error();
  DBUG_RETURN(0);
}


bool store_constraints(THD *thd, TABLE *table, const char *db,
                       const char *tname, const char *key_name,
                       uint key_len, const char *con_type, uint con_len)
{
  CHARSET_INFO *cs= system_charset_info;
  restore_record(table, s->default_values);
  table->field[1]->store(db, strlen(db), cs);
  table->field[2]->store(key_name, key_len, cs);
  table->field[3]->store(db, strlen(db), cs);
  table->field[4]->store(tname, strlen(tname), cs);
  table->field[5]->store(con_type, con_len, cs);
  return schema_table_store_record(thd, table);
}


static int get_schema_constraints_record(THD *thd, struct st_table_list *tables,
					 TABLE *table, bool res,
					 const char *base_name,
					 const char *file_name)
{
  DBUG_ENTER("get_schema_constraints_record");
  if (res)
  {
    if (!tables->view)
      push_warning(thd, MYSQL_ERROR::WARN_LEVEL_WARN,
                   thd->net.last_errno, thd->net.last_error);
    thd->clear_error();
    DBUG_RETURN(0);
  }
  else if (!tables->view)
  {
    List<FOREIGN_KEY_INFO> f_key_list;
    TABLE *show_table= tables->table;
    KEY *key_info=show_table->key_info;
    uint primary_key= show_table->s->primary_key;
    show_table->file->info(HA_STATUS_VARIABLE | 
                           HA_STATUS_NO_LOCK |
                           HA_STATUS_TIME);
    for (uint i=0 ; i < show_table->s->keys ; i++, key_info++)
    {
      if (i != primary_key && !(key_info->flags & HA_NOSAME))
        continue;

      if (i == primary_key && !strcmp(key_info->name, primary_key_name))
      {
        if (store_constraints(thd, table, base_name, file_name, key_info->name,
                              strlen(key_info->name),
                              STRING_WITH_LEN("PRIMARY KEY")))
          DBUG_RETURN(1);
      }
      else if (key_info->flags & HA_NOSAME)
      {
        if (store_constraints(thd, table, base_name, file_name, key_info->name,
                              strlen(key_info->name),
                              STRING_WITH_LEN("UNIQUE")))
          DBUG_RETURN(1);
      }
    }

    show_table->file->get_foreign_key_list(thd, &f_key_list);
    FOREIGN_KEY_INFO *f_key_info;
    List_iterator_fast<FOREIGN_KEY_INFO> it(f_key_list);
    while ((f_key_info=it++))
    {
      if (store_constraints(thd, table, base_name, file_name, 
                            f_key_info->forein_id->str,
                            strlen(f_key_info->forein_id->str),
                            "FOREIGN KEY", 11))
        DBUG_RETURN(1);
    }
  }
  DBUG_RETURN(res);
}


static bool store_trigger(THD *thd, TABLE *table, const char *db,
                          const char *tname, LEX_STRING *trigger_name,
                          enum trg_event_type event,
                          enum trg_action_time_type timing,
                          LEX_STRING *trigger_stmt,
                          ulong sql_mode,
                          LEX_STRING *definer_buffer,
                          LEX_STRING *client_cs_name,
                          LEX_STRING *connection_cl_name,
                          LEX_STRING *db_cl_name)
{
  CHARSET_INFO *cs= system_charset_info;
  LEX_STRING sql_mode_rep;

  restore_record(table, s->default_values);
  table->field[1]->store(db, strlen(db), cs);
  table->field[2]->store(trigger_name->str, trigger_name->length, cs);
  table->field[3]->store(trg_event_type_names[event].str,
                         trg_event_type_names[event].length, cs);
  table->field[5]->store(db, strlen(db), cs);
  table->field[6]->store(tname, strlen(tname), cs);
  table->field[9]->store(trigger_stmt->str, trigger_stmt->length, cs);
  table->field[10]->store(STRING_WITH_LEN("ROW"), cs);
  table->field[11]->store(trg_action_time_type_names[timing].str,
                          trg_action_time_type_names[timing].length, cs);
  table->field[14]->store(STRING_WITH_LEN("OLD"), cs);
  table->field[15]->store(STRING_WITH_LEN("NEW"), cs);

  sys_var_thd_sql_mode::symbolic_mode_representation(thd, sql_mode,
                                                     &sql_mode_rep);
  table->field[17]->store(sql_mode_rep.str, sql_mode_rep.length, cs);
  table->field[18]->store(definer_buffer->str, definer_buffer->length, cs);
  table->field[19]->store(client_cs_name->str, client_cs_name->length, cs);
  table->field[20]->store(connection_cl_name->str,
                          connection_cl_name->length, cs);
  table->field[21]->store(db_cl_name->str, db_cl_name->length, cs);

  return schema_table_store_record(thd, table);
}


static int get_schema_triggers_record(THD *thd, struct st_table_list *tables,
				      TABLE *table, bool res,
				      const char *base_name,
				      const char *file_name)
{
  DBUG_ENTER("get_schema_triggers_record");
  /*
    res can be non zero value when processed table is a view or
    error happened during opening of processed table.
  */
  if (res)
  {
    if (!tables->view)
      push_warning(thd, MYSQL_ERROR::WARN_LEVEL_WARN,
                   thd->net.last_errno, thd->net.last_error);
    thd->clear_error();
    DBUG_RETURN(0);
  }
  if (!tables->view && tables->table->triggers)
  {
    Table_triggers_list *triggers= tables->table->triggers;
    int event, timing;
    for (event= 0; event < (int)TRG_EVENT_MAX; event++)
    {
      for (timing= 0; timing < (int)TRG_ACTION_MAX; timing++)
      {
        LEX_STRING trigger_name;
        LEX_STRING trigger_stmt;
        ulong sql_mode;
        char definer_holder[USER_HOST_BUFF_SIZE];
        LEX_STRING definer_buffer;
        LEX_STRING client_cs_name;
        LEX_STRING connection_cl_name;
        LEX_STRING db_cl_name;

        definer_buffer.str= definer_holder;
        if (triggers->get_trigger_info(thd, (enum trg_event_type) event,
                                       (enum trg_action_time_type)timing,
                                       &trigger_name, &trigger_stmt,
                                       &sql_mode,
                                       &definer_buffer,
                                       &client_cs_name,
                                       &connection_cl_name,
                                       &db_cl_name))
          continue;

        if (store_trigger(thd, table, base_name, file_name, &trigger_name,
                         (enum trg_event_type) event,
                         (enum trg_action_time_type) timing, &trigger_stmt,
                         sql_mode,
                         &definer_buffer,
                         &client_cs_name,
                         &connection_cl_name,
                         &db_cl_name))
          DBUG_RETURN(1);
      }
    }
  }
  DBUG_RETURN(0);
}


void store_key_column_usage(TABLE *table, const char*db, const char *tname,
                            const char *key_name, uint key_len, 
                            const char *con_type, uint con_len, longlong idx)
{
  CHARSET_INFO *cs= system_charset_info;
  table->field[1]->store(db, strlen(db), cs);
  table->field[2]->store(key_name, key_len, cs);
  table->field[4]->store(db, strlen(db), cs);
  table->field[5]->store(tname, strlen(tname), cs);
  table->field[6]->store(con_type, con_len, cs);
  table->field[7]->store((longlong) idx, TRUE);
}


static int get_schema_key_column_usage_record(THD *thd,
					      struct st_table_list *tables,
					      TABLE *table, bool res,
					      const char *base_name,
					      const char *file_name)
{
  DBUG_ENTER("get_schema_key_column_usage_record");
  if (res)
  {
    if (!tables->view)
      push_warning(thd, MYSQL_ERROR::WARN_LEVEL_WARN,
                   thd->net.last_errno, thd->net.last_error);
    thd->clear_error();
    DBUG_RETURN(0);
  }
  else if (!tables->view)
  {
    List<FOREIGN_KEY_INFO> f_key_list;
    TABLE *show_table= tables->table;
    KEY *key_info=show_table->key_info;
    uint primary_key= show_table->s->primary_key;
    show_table->file->info(HA_STATUS_VARIABLE | 
                           HA_STATUS_NO_LOCK |
                           HA_STATUS_TIME);
    for (uint i=0 ; i < show_table->s->keys ; i++, key_info++)
    {
      if (i != primary_key && !(key_info->flags & HA_NOSAME))
        continue;
      uint f_idx= 0;
      KEY_PART_INFO *key_part= key_info->key_part;
      for (uint j=0 ; j < key_info->key_parts ; j++,key_part++)
      {
        if (key_part->field)
        {
          f_idx++;
          restore_record(table, s->default_values);
          store_key_column_usage(table, base_name, file_name,
                                 key_info->name,
                                 strlen(key_info->name), 
                                 key_part->field->field_name, 
                                 strlen(key_part->field->field_name),
                                 (longlong) f_idx);
          if (schema_table_store_record(thd, table))
            DBUG_RETURN(1);
        }
      }
    }

    show_table->file->get_foreign_key_list(thd, &f_key_list);
    FOREIGN_KEY_INFO *f_key_info;
    List_iterator_fast<FOREIGN_KEY_INFO> fkey_it(f_key_list);
    while ((f_key_info= fkey_it++))
    {
      LEX_STRING *f_info;
      LEX_STRING *r_info;
      List_iterator_fast<LEX_STRING> it(f_key_info->foreign_fields),
        it1(f_key_info->referenced_fields);
      uint f_idx= 0;
      while ((f_info= it++))
      {
        r_info= it1++;
        f_idx++;
        restore_record(table, s->default_values);
        store_key_column_usage(table, base_name, file_name,
                               f_key_info->forein_id->str,
                               f_key_info->forein_id->length,
                               f_info->str, f_info->length,
                               (longlong) f_idx);
        table->field[8]->store((longlong) f_idx, TRUE);
        table->field[8]->set_notnull();
        table->field[9]->store(f_key_info->referenced_db->str,
                               f_key_info->referenced_db->length,
                               system_charset_info);
        table->field[9]->set_notnull();
        table->field[10]->store(f_key_info->referenced_table->str,
                                f_key_info->referenced_table->length, 
                                system_charset_info);
        table->field[10]->set_notnull();
        table->field[11]->store(r_info->str, r_info->length,
                                system_charset_info);
        table->field[11]->set_notnull();
        if (schema_table_store_record(thd, table))
          DBUG_RETURN(1);
      }
    }
  }
  DBUG_RETURN(res);
}


#ifdef WITH_PARTITION_STORAGE_ENGINE
static void collect_partition_expr(List<char> &field_list, String *str)
{
  List_iterator<char> part_it(field_list);
  ulong no_fields= field_list.elements;
  const char *field_str;
  str->length(0);
  while ((field_str= part_it++))
  {
    str->append(field_str);
    if (--no_fields != 0)
      str->append(",");
  }
  return;
}
#endif


static void store_schema_partitions_record(THD *thd, TABLE *schema_table,
                                           TABLE *showing_table,
                                           partition_element *part_elem,
                                           handler *file, uint part_id)
{
  TABLE* table= schema_table;
  CHARSET_INFO *cs= system_charset_info;
  PARTITION_INFO stat_info;
  MYSQL_TIME time;
  file->get_dynamic_partition_info(&stat_info, part_id);
  table->field[12]->store((longlong) stat_info.records, TRUE);
  table->field[13]->store((longlong) stat_info.mean_rec_length, TRUE);
  table->field[14]->store((longlong) stat_info.data_file_length, TRUE);
  if (stat_info.max_data_file_length)
  {
    table->field[15]->store((longlong) stat_info.max_data_file_length, TRUE);
    table->field[15]->set_notnull();
  }
  table->field[16]->store((longlong) stat_info.index_file_length, TRUE);
  table->field[17]->store((longlong) stat_info.delete_length, TRUE);
  if (stat_info.create_time)
  {
    thd->variables.time_zone->gmt_sec_to_TIME(&time,
                                              (my_time_t)stat_info.create_time);
    table->field[18]->store_time(&time, MYSQL_TIMESTAMP_DATETIME);
    table->field[18]->set_notnull();
  }
  if (stat_info.update_time)
  {
    thd->variables.time_zone->gmt_sec_to_TIME(&time,
                                              (my_time_t)stat_info.update_time);
    table->field[19]->store_time(&time, MYSQL_TIMESTAMP_DATETIME);
    table->field[19]->set_notnull();
  }
  if (stat_info.check_time)
  {
    thd->variables.time_zone->gmt_sec_to_TIME(&time,
                                              (my_time_t)stat_info.check_time);
    table->field[20]->store_time(&time, MYSQL_TIMESTAMP_DATETIME);
    table->field[20]->set_notnull();
  }
  if (file->ha_table_flags() & (ulong) HA_HAS_CHECKSUM)
  {
    table->field[21]->store((longlong) stat_info.check_sum, TRUE);
    table->field[21]->set_notnull();
  }
  if (part_elem)
  {
    if (part_elem->part_comment)
      table->field[22]->store(part_elem->part_comment,
                              strlen(part_elem->part_comment), cs);
    else
      table->field[22]->store(STRING_WITH_LEN(""), cs);
    if (part_elem->nodegroup_id != UNDEF_NODEGROUP)
      table->field[23]->store((longlong) part_elem->nodegroup_id, TRUE);
    else
      table->field[23]->store(STRING_WITH_LEN("default"), cs);

    table->field[24]->set_notnull();
    if (part_elem->tablespace_name)
      table->field[24]->store(part_elem->tablespace_name,
                              strlen(part_elem->tablespace_name), cs);
    else
    {
      char *ts= showing_table->file->get_tablespace_name(thd,0,0);
      if(ts)
      {
        table->field[24]->store(ts, strlen(ts), cs);
        my_free(ts, MYF(0));
      }
      else
        table->field[24]->set_null();
    }
  }
  return;
}


static int get_schema_partitions_record(THD *thd, struct st_table_list *tables,
                                        TABLE *table, bool res,
                                        const char *base_name,
                                        const char *file_name)
{
  CHARSET_INFO *cs= system_charset_info;
  char buff[61];
  String tmp_res(buff, sizeof(buff), cs);
  String tmp_str;
  TABLE *show_table= tables->table;
  handler *file;
#ifdef WITH_PARTITION_STORAGE_ENGINE
  partition_info *part_info;
#endif
  DBUG_ENTER("get_schema_partitions_record");

  if (res)
  {
    if (!tables->view)
      push_warning(thd, MYSQL_ERROR::WARN_LEVEL_WARN,
                   thd->net.last_errno, thd->net.last_error);
    thd->clear_error();
    DBUG_RETURN(0);
  }
  file= show_table->file;
#ifdef WITH_PARTITION_STORAGE_ENGINE
  part_info= show_table->part_info;
  if (part_info)
  {
    partition_element *part_elem;
    List_iterator<partition_element> part_it(part_info->partitions);
    uint part_pos= 0, part_id= 0;

    restore_record(table, s->default_values);
    table->field[1]->store(base_name, strlen(base_name), cs);
    table->field[2]->store(file_name, strlen(file_name), cs);


    /* Partition method*/
    switch (part_info->part_type) {
    case RANGE_PARTITION:
      table->field[7]->store(partition_keywords[PKW_RANGE].str,
                             partition_keywords[PKW_RANGE].length, cs);
      break;
    case LIST_PARTITION:
      table->field[7]->store(partition_keywords[PKW_LIST].str,
                             partition_keywords[PKW_LIST].length, cs);
      break;
    case HASH_PARTITION:
      tmp_res.length(0);
      if (part_info->linear_hash_ind)
        tmp_res.append(partition_keywords[PKW_LINEAR].str,
                       partition_keywords[PKW_LINEAR].length);
      if (part_info->list_of_part_fields)
        tmp_res.append(partition_keywords[PKW_KEY].str,
                       partition_keywords[PKW_KEY].length);
      else
        tmp_res.append(partition_keywords[PKW_HASH].str, 
                       partition_keywords[PKW_HASH].length);
      table->field[7]->store(tmp_res.ptr(), tmp_res.length(), cs);
      break;
    default:
      DBUG_ASSERT(0);
      current_thd->fatal_error();
      DBUG_RETURN(1);
    }
    table->field[7]->set_notnull();

    /* Partition expression */
    if (part_info->part_expr)
    {
      table->field[9]->store(part_info->part_func_string,
                             part_info->part_func_len, cs);
    }
    else if (part_info->list_of_part_fields)
    {
      collect_partition_expr(part_info->part_field_list, &tmp_str);
      table->field[9]->store(tmp_str.ptr(), tmp_str.length(), cs);
    }
    table->field[9]->set_notnull();

    if (part_info->is_sub_partitioned())
    {
      /* Subpartition method */
      tmp_res.length(0);
      if (part_info->linear_hash_ind)
        tmp_res.append(partition_keywords[PKW_LINEAR].str,
                       partition_keywords[PKW_LINEAR].length);
      if (part_info->list_of_subpart_fields)
        tmp_res.append(partition_keywords[PKW_KEY].str,
                       partition_keywords[PKW_KEY].length);
      else
        tmp_res.append(partition_keywords[PKW_HASH].str, 
                       partition_keywords[PKW_HASH].length);
      table->field[8]->store(tmp_res.ptr(), tmp_res.length(), cs);
      table->field[8]->set_notnull();

      /* Subpartition expression */
      if (part_info->subpart_expr)
      {
        table->field[10]->store(part_info->subpart_func_string,
                                part_info->subpart_func_len, cs);
      }
      else if (part_info->list_of_subpart_fields)
      {
        collect_partition_expr(part_info->subpart_field_list, &tmp_str);
        table->field[10]->store(tmp_str.ptr(), tmp_str.length(), cs);
      }
      table->field[10]->set_notnull();
    }

    while ((part_elem= part_it++))
    {
      table->field[3]->store(part_elem->partition_name,
                             strlen(part_elem->partition_name), cs);
      table->field[3]->set_notnull();
      /* PARTITION_ORDINAL_POSITION */
      table->field[5]->store((longlong) ++part_pos, TRUE);
      table->field[5]->set_notnull();

      /* Partition description */
      if (part_info->part_type == RANGE_PARTITION)
      {
        if (part_elem->range_value != LONGLONG_MAX)
          table->field[11]->store((longlong) part_elem->range_value, FALSE);
        else
          table->field[11]->store(partition_keywords[PKW_MAXVALUE].str,
                                 partition_keywords[PKW_MAXVALUE].length, cs);
        table->field[11]->set_notnull();
      }
      else if (part_info->part_type == LIST_PARTITION)
      {
        List_iterator<part_elem_value> list_val_it(part_elem->list_val_list);
        part_elem_value *list_value;
        uint no_items= part_elem->list_val_list.elements;
        tmp_str.length(0);
        tmp_res.length(0);
        if (part_elem->has_null_value)
        {
          tmp_str.append("NULL");
          if (no_items > 0)
            tmp_str.append(",");
        }
        while ((list_value= list_val_it++))
        {
          if (!list_value->unsigned_flag)
            tmp_res.set(list_value->value, cs);
          else
            tmp_res.set((ulonglong)list_value->value, cs);
          tmp_str.append(tmp_res);
          if (--no_items != 0)
            tmp_str.append(",");
        };
        table->field[11]->store(tmp_str.ptr(), tmp_str.length(), cs);
        table->field[11]->set_notnull();
      }

      if (part_elem->subpartitions.elements)
      {
        List_iterator<partition_element> sub_it(part_elem->subpartitions);
        partition_element *subpart_elem;
        uint subpart_pos= 0;

        while ((subpart_elem= sub_it++))
        {
          table->field[4]->store(subpart_elem->partition_name,
                                 strlen(subpart_elem->partition_name), cs);
          table->field[4]->set_notnull();
          /* SUBPARTITION_ORDINAL_POSITION */
          table->field[6]->store((longlong) ++subpart_pos, TRUE);
          table->field[6]->set_notnull();
          
          store_schema_partitions_record(thd, table, show_table, subpart_elem,
                                         file, part_id);
          part_id++;
          if(schema_table_store_record(thd, table))
            DBUG_RETURN(1);
        }
      }
      else
      {
        store_schema_partitions_record(thd, table, show_table, part_elem,
                                       file, part_id);
        part_id++;
        if(schema_table_store_record(thd, table))
          DBUG_RETURN(1);
      }
    }
    DBUG_RETURN(0);
  }
  else
#endif
  {
    store_schema_partitions_record(thd, table, show_table, 0, file, 0);
    if(schema_table_store_record(thd, table))
      DBUG_RETURN(1);
  }
  DBUG_RETURN(0);
}


#ifdef NOT_USED
static interval_type get_real_interval_type(interval_type i_type)
{
  switch (i_type) {
  case INTERVAL_YEAR:
    return INTERVAL_YEAR;

  case INTERVAL_QUARTER:
  case INTERVAL_YEAR_MONTH:
  case INTERVAL_MONTH:
    return INTERVAL_MONTH;

  case INTERVAL_WEEK:
  case INTERVAL_DAY:
    return INTERVAL_DAY;

  case INTERVAL_DAY_HOUR:
  case INTERVAL_HOUR:
    return INTERVAL_HOUR;

  case INTERVAL_DAY_MINUTE:
  case INTERVAL_HOUR_MINUTE:
  case INTERVAL_MINUTE:
    return INTERVAL_MINUTE;

  case INTERVAL_DAY_SECOND:
  case INTERVAL_HOUR_SECOND:
  case INTERVAL_MINUTE_SECOND:
  case INTERVAL_SECOND:
    return INTERVAL_SECOND;

  case INTERVAL_DAY_MICROSECOND:
  case INTERVAL_HOUR_MICROSECOND:
  case INTERVAL_MINUTE_MICROSECOND:
  case INTERVAL_SECOND_MICROSECOND:
  case INTERVAL_MICROSECOND:
    return INTERVAL_MICROSECOND;
  case INTERVAL_LAST:
    DBUG_ASSERT(0);
  }
  DBUG_ASSERT(0);
  return INTERVAL_SECOND;
}

#endif


/*
  Loads an event from mysql.event and copies it's data to a row of
  I_S.EVENTS

  Synopsis
    copy_event_to_schema_table()
      thd         Thread
      sch_table   The schema table (information_schema.event)
      event_table The event table to use for loading (mysql.event).

  Returns
    0  OK
    1  Error
*/

int
copy_event_to_schema_table(THD *thd, TABLE *sch_table, TABLE *event_table)
{
  const char *wild= thd->lex->wild ? thd->lex->wild->ptr() : NullS;
  CHARSET_INFO *scs= system_charset_info;
  MYSQL_TIME time;
  Event_timed et;
  DBUG_ENTER("copy_event_to_schema_table");

  restore_record(sch_table, s->default_values);

  if (et.load_from_row(thd, event_table))
  {
    my_error(ER_CANNOT_LOAD_FROM_TABLE, MYF(0), event_table->alias);
    DBUG_RETURN(1);
  }

  if (!(!wild || !wild[0] || !wild_compare(et.name.str, wild, 0)))
    DBUG_RETURN(0);

  /*
    Skip events in schemas one does not have access to. The check is
    optimized. It's guaranteed in case of SHOW EVENTS that the user
    has access.
  */
  if (thd->lex->sql_command != SQLCOM_SHOW_EVENTS &&
      check_access(thd, EVENT_ACL, et.dbname.str, 0, 0, 1,
                   is_schema_db(et.dbname.str)))
    DBUG_RETURN(0);

  /* ->field[0] is EVENT_CATALOG and is by default NULL */

  sch_table->field[ISE_EVENT_SCHEMA]->
                                store(et.dbname.str, et.dbname.length,scs);
  sch_table->field[ISE_EVENT_NAME]->
                                store(et.name.str, et.name.length, scs);
  sch_table->field[ISE_DEFINER]->
                                store(et.definer.str, et.definer.length, scs);
  const String *tz_name= et.time_zone->get_name();
  sch_table->field[ISE_TIME_ZONE]->
                                store(tz_name->ptr(), tz_name->length(), scs);
  sch_table->field[ISE_EVENT_BODY]->
                                store(STRING_WITH_LEN("SQL"), scs);
  sch_table->field[ISE_EVENT_DEFINITION]->store(
    et.body_utf8.str, et.body_utf8.length, scs);

  /* SQL_MODE */
  {
    LEX_STRING sql_mode;
    sys_var_thd_sql_mode::symbolic_mode_representation(thd, et.sql_mode,
                                                       &sql_mode);
    sch_table->field[ISE_SQL_MODE]->
                                store(sql_mode.str, sql_mode.length, scs);
  }

  int not_used=0;

  if (et.expression)
  {
    String show_str;
    /* type */
    sch_table->field[ISE_EVENT_TYPE]->store(STRING_WITH_LEN("RECURRING"), scs);

    if (Events::reconstruct_interval_expression(&show_str, et.interval,
                                                et.expression))
      DBUG_RETURN(1);

    sch_table->field[ISE_INTERVAL_VALUE]->set_notnull();
    sch_table->field[ISE_INTERVAL_VALUE]->
                                store(show_str.ptr(), show_str.length(), scs);

    LEX_STRING *ival= &interval_type_to_name[et.interval];
    sch_table->field[ISE_INTERVAL_FIELD]->set_notnull();
    sch_table->field[ISE_INTERVAL_FIELD]->store(ival->str, ival->length, scs);

    /* starts & ends . STARTS is always set - see sql_yacc.yy */
    et.time_zone->gmt_sec_to_TIME(&time, et.starts);
    sch_table->field[ISE_STARTS]->set_notnull();
    sch_table->field[ISE_STARTS]->
                                store_time(&time, MYSQL_TIMESTAMP_DATETIME);

    if (!et.ends_null)
    {
      et.time_zone->gmt_sec_to_TIME(&time, et.ends);
      sch_table->field[ISE_ENDS]->set_notnull();
      sch_table->field[ISE_ENDS]->
                                store_time(&time, MYSQL_TIMESTAMP_DATETIME);
    }
  }
  else
  {
    /* type */
    sch_table->field[ISE_EVENT_TYPE]->store(STRING_WITH_LEN("ONE TIME"), scs);

    et.time_zone->gmt_sec_to_TIME(&time, et.execute_at);
    sch_table->field[ISE_EXECUTE_AT]->set_notnull();
    sch_table->field[ISE_EXECUTE_AT]->
                          store_time(&time, MYSQL_TIMESTAMP_DATETIME);
  }

  /* status */

  switch (et.status)
  {
    case Event_timed::ENABLED:
      sch_table->field[ISE_STATUS]->store(STRING_WITH_LEN("ENABLED"), scs);
      break;
    case Event_timed::SLAVESIDE_DISABLED:
      sch_table->field[ISE_STATUS]->store(STRING_WITH_LEN("SLAVESIDE_DISABLED"),
                                          scs);
      break;
    case Event_timed::DISABLED:
      sch_table->field[ISE_STATUS]->store(STRING_WITH_LEN("DISABLED"), scs);
      break;
    default:
      DBUG_ASSERT(0);
  }
  sch_table->field[ISE_ORIGINATOR]->store(et.originator, TRUE);

  /* on_completion */
  if (et.on_completion == Event_timed::ON_COMPLETION_DROP)
    sch_table->field[ISE_ON_COMPLETION]->
                                store(STRING_WITH_LEN("NOT PRESERVE"), scs);
  else
    sch_table->field[ISE_ON_COMPLETION]->
                                store(STRING_WITH_LEN("PRESERVE"), scs);
    
  number_to_datetime(et.created, &time, 0, &not_used);
  DBUG_ASSERT(not_used==0);
  sch_table->field[ISE_CREATED]->store_time(&time, MYSQL_TIMESTAMP_DATETIME);

  number_to_datetime(et.modified, &time, 0, &not_used);
  DBUG_ASSERT(not_used==0);
  sch_table->field[ISE_LAST_ALTERED]->
                                store_time(&time, MYSQL_TIMESTAMP_DATETIME);

  if (et.last_executed)
  {
    et.time_zone->gmt_sec_to_TIME(&time, et.last_executed);
    sch_table->field[ISE_LAST_EXECUTED]->set_notnull();
    sch_table->field[ISE_LAST_EXECUTED]->
                       store_time(&time, MYSQL_TIMESTAMP_DATETIME);
  }

  sch_table->field[ISE_EVENT_COMMENT]->
                      store(et.comment.str, et.comment.length, scs);

  sch_table->field[ISE_CLIENT_CS]->set_notnull();
  sch_table->field[ISE_CLIENT_CS]->store(
    et.creation_ctx->get_client_cs()->csname,
    strlen(et.creation_ctx->get_client_cs()->csname),
    scs);

  sch_table->field[ISE_CONNECTION_CL]->set_notnull();
  sch_table->field[ISE_CONNECTION_CL]->store(
    et.creation_ctx->get_connection_cl()->name,
    strlen(et.creation_ctx->get_connection_cl()->name),
    scs);

  sch_table->field[ISE_DB_CL]->set_notnull();
  sch_table->field[ISE_DB_CL]->store(
    et.creation_ctx->get_db_cl()->name,
    strlen(et.creation_ctx->get_db_cl()->name),
    scs);

  if (schema_table_store_record(thd, sch_table))
    DBUG_RETURN(1);

  DBUG_RETURN(0);
}


int fill_open_tables(THD *thd, TABLE_LIST *tables, COND *cond)
{
  DBUG_ENTER("fill_open_tables");
  const char *wild= thd->lex->wild ? thd->lex->wild->ptr() : NullS;
  TABLE *table= tables->table;
  CHARSET_INFO *cs= system_charset_info;
  OPEN_TABLE_LIST *open_list;
  if (!(open_list=list_open_tables(thd,thd->lex->select_lex.db, wild))
            && thd->is_fatal_error)
    DBUG_RETURN(1);

  for (; open_list ; open_list=open_list->next)
  {
    restore_record(table, s->default_values);
    table->field[0]->store(open_list->db, strlen(open_list->db), cs);
    table->field[1]->store(open_list->table, strlen(open_list->table), cs);
    table->field[2]->store((longlong) open_list->in_use, TRUE);
    table->field[3]->store((longlong) open_list->locked, TRUE);
    if (schema_table_store_record(thd, table))
      DBUG_RETURN(1);
  }
  DBUG_RETURN(0);
}


int fill_variables(THD *thd, TABLE_LIST *tables, COND *cond)
{
  DBUG_ENTER("fill_variables");
  int res= 0;
  LEX *lex= thd->lex;
  const char *wild= lex->wild ? lex->wild->ptr() : NullS;
  enum enum_schema_tables schema_table_idx=
    get_schema_table_idx(tables->schema_table);
  enum enum_var_type option_type= OPT_SESSION;
  bool upper_case_names= (schema_table_idx != SCH_VARIABLES);
  bool sorted_vars= (schema_table_idx == SCH_VARIABLES);

  if (lex->option_type == OPT_GLOBAL ||
      schema_table_idx == SCH_GLOBAL_VARIABLES)
    option_type= OPT_GLOBAL;

  rw_rdlock(&LOCK_system_variables_hash);
  res= show_status_array(thd, wild, enumerate_sys_vars(thd, sorted_vars),
                         option_type, NULL, "", tables->table, upper_case_names);
  rw_unlock(&LOCK_system_variables_hash);
  DBUG_RETURN(res);
}


int fill_status(THD *thd, TABLE_LIST *tables, COND *cond)
{
  DBUG_ENTER("fill_status");
  LEX *lex= thd->lex;
  const char *wild= lex->wild ? lex->wild->ptr() : NullS;
  int res= 0;
  STATUS_VAR *tmp1, tmp;
  enum enum_schema_tables schema_table_idx=
    get_schema_table_idx(tables->schema_table);
  enum enum_var_type option_type;
  bool upper_case_names= (schema_table_idx != SCH_STATUS);

  if (schema_table_idx == SCH_STATUS)
  {
    option_type= lex->option_type;
    if (option_type == OPT_GLOBAL)
      tmp1= &tmp;
    else
      tmp1= thd->initial_status_var;
  }
  else if (schema_table_idx == SCH_GLOBAL_STATUS)
  {
    option_type= OPT_GLOBAL;
    tmp1= &tmp;
  }
  else
  { 
    option_type= OPT_SESSION;
    tmp1= &thd->status_var;
  }

  pthread_mutex_lock(&LOCK_status);
  if (option_type == OPT_GLOBAL)
    calc_sum_of_all_status(&tmp);
  res= show_status_array(thd, wild,
                         (SHOW_VAR *)all_status_vars.buffer,
                         option_type, tmp1, "", tables->table,
                         upper_case_names);
  pthread_mutex_unlock(&LOCK_status);
  DBUG_RETURN(res);
}


/*
  Fill and store records into I_S.referential_constraints table

  SYNOPSIS
    get_referential_constraints_record()
    thd                 thread handle
    tables              table list struct(processed table)
    table               I_S table
    res                 1 means the error during opening of the processed table
                        0 means processed table is opened without error
    base_name           db name
    file_name           table name

  RETURN
    0	ok
    #   error
*/

static int
get_referential_constraints_record(THD *thd, struct st_table_list *tables,
                                   TABLE *table, bool res,
                                   const char *base_name, const char *file_name)
{
  CHARSET_INFO *cs= system_charset_info;
  DBUG_ENTER("get_referential_constraints_record");

  if (res)
  {
    if (!tables->view)
      push_warning(thd, MYSQL_ERROR::WARN_LEVEL_WARN,
                   thd->net.last_errno, thd->net.last_error);
    thd->clear_error();
    DBUG_RETURN(0);
  }
  if (!tables->view)
  {
    List<FOREIGN_KEY_INFO> f_key_list;
    TABLE *show_table= tables->table;
    show_table->file->info(HA_STATUS_VARIABLE | 
                           HA_STATUS_NO_LOCK |
                           HA_STATUS_TIME);

    show_table->file->get_foreign_key_list(thd, &f_key_list);
    FOREIGN_KEY_INFO *f_key_info;
    List_iterator_fast<FOREIGN_KEY_INFO> it(f_key_list);
    while ((f_key_info= it++))
    {
      restore_record(table, s->default_values);
      table->field[1]->store(base_name, strlen(base_name), cs);
      table->field[9]->store(file_name, strlen(file_name), cs);
      table->field[2]->store(f_key_info->forein_id->str,
                             f_key_info->forein_id->length, cs);
      table->field[4]->store(f_key_info->referenced_db->str, 
                             f_key_info->referenced_db->length, cs);
      table->field[10]->store(f_key_info->referenced_table->str, 
                             f_key_info->referenced_table->length, cs);
      table->field[5]->store(f_key_info->referenced_key_name->str, 
                             f_key_info->referenced_key_name->length, cs);
      table->field[6]->store(STRING_WITH_LEN("NONE"), cs);
      table->field[7]->store(f_key_info->update_method->str, 
                             f_key_info->update_method->length, cs);
      table->field[8]->store(f_key_info->delete_method->str, 
                             f_key_info->delete_method->length, cs);
      if (schema_table_store_record(thd, table))
        DBUG_RETURN(1);
    }
  }
  DBUG_RETURN(0);
}

struct schema_table_ref 
{
  const char *table_name;
  ST_SCHEMA_TABLE *schema_table;
};


/*
  Find schema_tables elment by name

  SYNOPSIS
    find_schema_table_in_plugin()
    thd                 thread handler
    plugin              plugin
    table_name          table name

  RETURN
    0	table not found
    1   found the schema table
*/
static my_bool find_schema_table_in_plugin(THD *thd, plugin_ref plugin,
                                           void* p_table)
{
  schema_table_ref *p_schema_table= (schema_table_ref *)p_table;
  const char* table_name= p_schema_table->table_name;
  ST_SCHEMA_TABLE *schema_table= plugin_data(plugin, ST_SCHEMA_TABLE *);
  DBUG_ENTER("find_schema_table_in_plugin");

  if (!my_strcasecmp(system_charset_info,
                     schema_table->table_name,
                     table_name)) {
    p_schema_table->schema_table= schema_table;
    DBUG_RETURN(1);
  }

  DBUG_RETURN(0);
}


/*
  Find schema_tables elment by name

  SYNOPSIS
    find_schema_table()
    thd                 thread handler
    table_name          table name

  RETURN
    0	table not found
    #   pointer to 'shema_tables' element
*/

ST_SCHEMA_TABLE *find_schema_table(THD *thd, const char* table_name)
{
  schema_table_ref schema_table_a;
  ST_SCHEMA_TABLE *schema_table= schema_tables;
  DBUG_ENTER("find_schema_table");

  for (; schema_table->table_name; schema_table++)
  {
    if (!my_strcasecmp(system_charset_info,
                       schema_table->table_name,
                       table_name))
      DBUG_RETURN(schema_table);
  }

  schema_table_a.table_name= table_name;
  if (plugin_foreach(thd, find_schema_table_in_plugin, 
                     MYSQL_INFORMATION_SCHEMA_PLUGIN, &schema_table_a))
    DBUG_RETURN(schema_table_a.schema_table);

  DBUG_RETURN(NULL);
}


ST_SCHEMA_TABLE *get_schema_table(enum enum_schema_tables schema_table_idx)
{
  return &schema_tables[schema_table_idx];
}


/*
  Create information_schema table using schema_table data

  SYNOPSIS
    create_schema_table()
    thd	       	          thread handler
    schema_table          pointer to 'shema_tables' element

  RETURN
    #	                  Pointer to created table
    0	                  Can't create table
*/

TABLE *create_schema_table(THD *thd, TABLE_LIST *table_list)
{
  int field_count= 0;
  Item *item;
  TABLE *table;
  List<Item> field_list;
  ST_SCHEMA_TABLE *schema_table= table_list->schema_table;
  ST_FIELD_INFO *fields_info= schema_table->fields_info;
  CHARSET_INFO *cs= system_charset_info;
  DBUG_ENTER("create_schema_table");

  for (; fields_info->field_name; fields_info++)
  {
    switch (fields_info->field_type) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_LONGLONG:
    case MYSQL_TYPE_INT24:
      if (!(item= new Item_return_int(fields_info->field_name,
                                      fields_info->field_length,
                                      fields_info->field_type,
                                      fields_info->value)))
      {
        DBUG_RETURN(0);
      }
      item->unsigned_flag= (fields_info->field_flags & MY_I_S_UNSIGNED);
      break;
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_TIME:
    case MYSQL_TYPE_TIMESTAMP:
    case MYSQL_TYPE_DATETIME:
      if (!(item=new Item_return_date_time(fields_info->field_name,
                                           fields_info->field_type)))
      {
        DBUG_RETURN(0);
      }
      break;
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE:
      if ((item= new Item_float(fields_info->field_name, 0.0, NOT_FIXED_DEC, 
                           fields_info->field_length)) == NULL)
        DBUG_RETURN(NULL);
      break;
    case MYSQL_TYPE_DECIMAL:
      if (!(item= new Item_decimal((longlong) fields_info->value, false)))
      {
        DBUG_RETURN(0);
      }
      item->unsigned_flag= (fields_info->field_flags & MY_I_S_UNSIGNED);
      item->decimals= fields_info->field_length%10;
      item->max_length= (fields_info->field_length/100)%100;
      if (item->unsigned_flag == 0)
        item->max_length+= 1;
      if (item->decimals > 0)
        item->max_length+= 1;
      item->set_name(fields_info->field_name,
                     strlen(fields_info->field_name), cs);
      break;
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_BLOB:
      if (!(item= new Item_blob(fields_info->field_name,
                                fields_info->field_length)))
      {
        DBUG_RETURN(0);
      }
      break;
    default:
      /* Don't let unimplemented types pass through. Could be a grave error. */
      DBUG_ASSERT(fields_info->field_type == MYSQL_TYPE_STRING);

      if (!(item= new Item_empty_string("", fields_info->field_length, cs)))
      {
        DBUG_RETURN(0);
      }
      item->set_name(fields_info->field_name,
                     strlen(fields_info->field_name), cs);
      break;
    }
    field_list.push_back(item);
    item->maybe_null= (fields_info->field_flags & MY_I_S_MAYBE_NULL);
    field_count++;
  }
  TMP_TABLE_PARAM *tmp_table_param =
    (TMP_TABLE_PARAM*) (thd->alloc(sizeof(TMP_TABLE_PARAM)));
  tmp_table_param->init();
  tmp_table_param->table_charset= cs;
  tmp_table_param->field_count= field_count;
  tmp_table_param->schema_table= 1;
  SELECT_LEX *select_lex= thd->lex->current_select;
  if (!(table= create_tmp_table(thd, tmp_table_param,
                                field_list, (ORDER*) 0, 0, 0, 
                                (select_lex->options | thd->options |
                                 TMP_TABLE_ALL_COLUMNS),
                                HA_POS_ERROR, table_list->alias)))
    DBUG_RETURN(0);
  table_list->schema_table_param= tmp_table_param;
  DBUG_RETURN(table);
}


/*
  For old SHOW compatibility. It is used when
  old SHOW doesn't have generated column names
  Make list of fields for SHOW

  SYNOPSIS
    make_old_format()
    thd			thread handler
    schema_table        pointer to 'schema_tables' element

  RETURN
   1	error
   0	success
*/

int make_old_format(THD *thd, ST_SCHEMA_TABLE *schema_table)
{
  ST_FIELD_INFO *field_info= schema_table->fields_info;
  Name_resolution_context *context= &thd->lex->select_lex.context;
  for (; field_info->field_name; field_info++)
  {
    if (field_info->old_name)
    {
      Item_field *field= new Item_field(context,
                                        NullS, NullS, field_info->field_name);
      if (field)
      {
        field->set_name(field_info->old_name,
                        strlen(field_info->old_name),
                        system_charset_info);
        if (add_item_to_list(thd, field))
          return 1;
      }
    }
  }
  return 0;
}


int make_schemata_old_format(THD *thd, ST_SCHEMA_TABLE *schema_table)
{
  char tmp[128];
  LEX *lex= thd->lex;
  SELECT_LEX *sel= lex->current_select;
  Name_resolution_context *context= &sel->context;

  if (!sel->item_list.elements)
  {
    ST_FIELD_INFO *field_info= &schema_table->fields_info[1];
    String buffer(tmp,sizeof(tmp), system_charset_info);
    Item_field *field= new Item_field(context,
                                      NullS, NullS, field_info->field_name);
    if (!field || add_item_to_list(thd, field))
      return 1;
    buffer.length(0);
    buffer.append(field_info->old_name);
    if (lex->wild && lex->wild->ptr())
    {
      buffer.append(STRING_WITH_LEN(" ("));
      buffer.append(lex->wild->ptr());
      buffer.append(')');
    }
    field->set_name(buffer.ptr(), buffer.length(), system_charset_info);
  }
  return 0;
}


int make_table_names_old_format(THD *thd, ST_SCHEMA_TABLE *schema_table)
{
  char tmp[128];
  String buffer(tmp,sizeof(tmp), thd->charset());
  LEX *lex= thd->lex;
  Name_resolution_context *context= &lex->select_lex.context;

  ST_FIELD_INFO *field_info= &schema_table->fields_info[2];
  buffer.length(0);
  buffer.append(field_info->old_name);
  buffer.append(lex->select_lex.db);
  if (lex->wild && lex->wild->ptr())
  {
    buffer.append(STRING_WITH_LEN(" ("));
    buffer.append(lex->wild->ptr());
    buffer.append(')');
  }
  Item_field *field= new Item_field(context,
                                    NullS, NullS, field_info->field_name);
  if (add_item_to_list(thd, field))
    return 1;
  field->set_name(buffer.ptr(), buffer.length(), system_charset_info);
  if (thd->lex->verbose)
  {
    field->set_name(buffer.ptr(), buffer.length(), system_charset_info);
    field_info= &schema_table->fields_info[3];
    field= new Item_field(context, NullS, NullS, field_info->field_name);
    if (add_item_to_list(thd, field))
      return 1;
    field->set_name(field_info->old_name, strlen(field_info->old_name),
                    system_charset_info);
  }
  return 0;
}


int make_columns_old_format(THD *thd, ST_SCHEMA_TABLE *schema_table)
{
  int fields_arr[]= {3, 14, 13, 6, 15, 5, 16, 17, 18, -1};
  int *field_num= fields_arr;
  ST_FIELD_INFO *field_info;
  Name_resolution_context *context= &thd->lex->select_lex.context;

  for (; *field_num >= 0; field_num++)
  {
    field_info= &schema_table->fields_info[*field_num];
    if (!thd->lex->verbose && (*field_num == 13 ||
                               *field_num == 17 ||
                               *field_num == 18))
      continue;
    Item_field *field= new Item_field(context,
                                      NullS, NullS, field_info->field_name);
    if (field)
    {
      field->set_name(field_info->old_name,
                      strlen(field_info->old_name),
                      system_charset_info);
      if (add_item_to_list(thd, field))
        return 1;
    }
  }
  return 0;
}


int make_character_sets_old_format(THD *thd, ST_SCHEMA_TABLE *schema_table)
{
  int fields_arr[]= {0, 2, 1, 3, -1};
  int *field_num= fields_arr;
  ST_FIELD_INFO *field_info;
  Name_resolution_context *context= &thd->lex->select_lex.context;

  for (; *field_num >= 0; field_num++)
  {
    field_info= &schema_table->fields_info[*field_num];
    Item_field *field= new Item_field(context,
                                      NullS, NullS, field_info->field_name);
    if (field)
    {
      field->set_name(field_info->old_name,
                      strlen(field_info->old_name),
                      system_charset_info);
      if (add_item_to_list(thd, field))
        return 1;
    }
  }
  return 0;
}


int make_proc_old_format(THD *thd, ST_SCHEMA_TABLE *schema_table)
{
  int fields_arr[]= {2, 3, 4, 19, 16, 15, 14, 18, 20, 21, 22, -1};
  int *field_num= fields_arr;
  ST_FIELD_INFO *field_info;
  Name_resolution_context *context= &thd->lex->select_lex.context;

  for (; *field_num >= 0; field_num++)
  {
    field_info= &schema_table->fields_info[*field_num];
    Item_field *field= new Item_field(context,
                                      NullS, NullS, field_info->field_name);
    if (field)
    {
      field->set_name(field_info->old_name,
                      strlen(field_info->old_name),
                      system_charset_info);
      if (add_item_to_list(thd, field))
        return 1;
    }
  }
  return 0;
}


/*
  Create information_schema table

  SYNOPSIS
  mysql_schema_table()
    thd                thread handler
    lex                pointer to LEX
    table_list         pointer to table_list

  RETURN
    0	success
    1   error
*/

int mysql_schema_table(THD *thd, LEX *lex, TABLE_LIST *table_list)
{
  TABLE *table;
  DBUG_ENTER("mysql_schema_table");
  if (!(table= table_list->schema_table->create_table(thd, table_list)))
    DBUG_RETURN(1);
  table->s->tmp_table= SYSTEM_TMP_TABLE;
  table->grant.privilege= SELECT_ACL;
  /*
    This test is necessary to make
    case insensitive file systems +
    upper case table names(information schema tables) +
    views
    working correctly
  */
  if (table_list->schema_table_name)
    table->alias_name_used= my_strcasecmp(table_alias_charset,
                                          table_list->schema_table_name,
                                          table_list->alias);
  table_list->table_name= table->s->table_name.str;
  table_list->table_name_length= table->s->table_name.length;
  table_list->table= table;
  table->next= thd->derived_tables;
  thd->derived_tables= table;
  table_list->select_lex->options |= OPTION_SCHEMA_TABLE;
  lex->safe_to_cache_query= 0;

  if (table_list->schema_table_reformed) // show command
  {
    SELECT_LEX *sel= lex->current_select;
    Item *item;
    Field_translator *transl, *org_transl;

    if (table_list->field_translation)
    {
      Field_translator *end= table_list->field_translation_end;
      for (transl= table_list->field_translation; transl < end; transl++)
      {
        if (!transl->item->fixed &&
            transl->item->fix_fields(thd, &transl->item))
          DBUG_RETURN(1);
      }
      DBUG_RETURN(0);
    }
    List_iterator_fast<Item> it(sel->item_list);
    if (!(transl=
          (Field_translator*)(thd->stmt_arena->
                              alloc(sel->item_list.elements *
                                    sizeof(Field_translator)))))
    {
      DBUG_RETURN(1);
    }
    for (org_transl= transl; (item= it++); transl++)
    {
      transl->item= item;
      transl->name= item->name;
      if (!item->fixed && item->fix_fields(thd, &transl->item))
      {
        DBUG_RETURN(1);
      }
    }
    table_list->field_translation= org_transl;
    table_list->field_translation_end= transl;
  }

  DBUG_RETURN(0);
}


/*
  Generate select from information_schema table

  SYNOPSIS
    make_schema_select()
    thd                  thread handler
    sel                  pointer to SELECT_LEX
    schema_table_idx     index of 'schema_tables' element

  RETURN
    0	success
    1   error
*/

int make_schema_select(THD *thd, SELECT_LEX *sel,
		       enum enum_schema_tables schema_table_idx)
{
  ST_SCHEMA_TABLE *schema_table= get_schema_table(schema_table_idx);
  LEX_STRING db, table;
  DBUG_ENTER("mysql_schema_select");
  DBUG_PRINT("enter", ("mysql_schema_select: %s", schema_table->table_name));
  /*
     We have to make non const db_name & table_name
     because of lower_case_table_names
  */
  make_lex_string(thd, &db, INFORMATION_SCHEMA_NAME.str,
                  INFORMATION_SCHEMA_NAME.length, 0);
  make_lex_string(thd, &table, schema_table->table_name,
                  strlen(schema_table->table_name), 0);
  if (schema_table->old_format(thd, schema_table) ||   /* Handle old syntax */
      !sel->add_table_to_list(thd, new Table_ident(thd, db, table, 0),
                              0, 0, TL_READ))
  {
    DBUG_RETURN(1);
  }
  DBUG_RETURN(0);
}


/*
  Fill temporary schema tables before SELECT

  SYNOPSIS
    get_schema_tables_result()
    join  join which use schema tables
    executed_place place where I_S table processed

  RETURN
    FALSE success
    TRUE  error
*/

bool get_schema_tables_result(JOIN *join,
                              enum enum_schema_table_state executed_place)
{
  JOIN_TAB *tmp_join_tab= join->join_tab+join->tables;
  THD *thd= join->thd;
  LEX *lex= thd->lex;
  bool result= 0;
  DBUG_ENTER("get_schema_tables_result");

  thd->no_warnings_for_error= 1;
  for (JOIN_TAB *tab= join->join_tab; tab < tmp_join_tab; tab++)
  {  
    if (!tab->table || !tab->table->pos_in_table_list)
      break;

    TABLE_LIST *table_list= tab->table->pos_in_table_list;
    if (table_list->schema_table && thd->fill_information_schema_tables())
    {
      bool is_subselect= (&lex->unit != lex->current_select->master_unit() &&
                          lex->current_select->master_unit()->item);
      /*
        If schema table is already processed and
        the statement is not a subselect then
        we don't need to fill this table again.
        If schema table is already processed and
        schema_table_state != executed_place then
        table is already processed and
        we should skip second data processing.
      */
      if (table_list->schema_table_state &&
          (!is_subselect || table_list->schema_table_state != executed_place))
        continue;

      /*
        if table is used in a subselect and
        table has been processed earlier with the same
        'executed_place' value then we should refresh the table.
      */
      if (table_list->schema_table_state && is_subselect)
      {
        table_list->table->file->extra(HA_EXTRA_NO_CACHE);
        table_list->table->file->extra(HA_EXTRA_RESET_STATE);
        table_list->table->file->delete_all_rows();
        free_io_cache(table_list->table);
        filesort_free_buffers(table_list->table,1);
        table_list->table->null_row= 0;
      }
      else
        table_list->table->file->stats.records= 0;

      if (table_list->schema_table->fill_table(thd, table_list,
                                               tab->select_cond))
      {
        result= 1;
        join->error= 1;
        table_list->schema_table_state= executed_place;
        break;
      }
      table_list->schema_table_state= executed_place;
    }
  }
  thd->no_warnings_for_error= 0;
  DBUG_RETURN(result);
}

struct run_hton_fill_schema_files_args
{
  TABLE_LIST *tables;
  COND *cond;
};

static my_bool run_hton_fill_schema_files(THD *thd, plugin_ref plugin,
                                          void *arg)
{
  struct run_hton_fill_schema_files_args *args=
    (run_hton_fill_schema_files_args *) arg;
  handlerton *hton= plugin_data(plugin, handlerton *);
  if(hton->fill_files_table && hton->state == SHOW_OPTION_YES)
    hton->fill_files_table(hton, thd, args->tables, args->cond);
  return false;
}

int fill_schema_files(THD *thd, TABLE_LIST *tables, COND *cond)
{
  DBUG_ENTER("fill_schema_files");

  struct run_hton_fill_schema_files_args args;
  args.tables= tables;
  args.cond= cond;

  plugin_foreach(thd, run_hton_fill_schema_files,
                 MYSQL_STORAGE_ENGINE_PLUGIN, &args);

  DBUG_RETURN(0);
}


ST_FIELD_INFO schema_fields_info[]=
{
  {"CATALOG_NAME", FN_REFLEN, MYSQL_TYPE_STRING, 0, 1, 0},
  {"SCHEMA_NAME", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, "Database"},
  {"DEFAULT_CHARACTER_SET_NAME", 64, MYSQL_TYPE_STRING, 0, 0, 0},
  {"DEFAULT_COLLATION_NAME", 64, MYSQL_TYPE_STRING, 0, 0, 0},
  {"SQL_PATH", FN_REFLEN, MYSQL_TYPE_STRING, 0, 1, 0},
  {0, 0, MYSQL_TYPE_STRING, 0, 0, 0}
};


ST_FIELD_INFO tables_fields_info[]=
{
  {"TABLE_CATALOG", FN_REFLEN, MYSQL_TYPE_STRING, 0, 1, 0},
  {"TABLE_SCHEMA", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {"TABLE_NAME", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, "Name"},
  {"TABLE_TYPE", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {"ENGINE", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 1, "Engine"},
  {"VERSION", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
   (MY_I_S_MAYBE_NULL | MY_I_S_UNSIGNED), "Version"},
  {"ROW_FORMAT", 10, MYSQL_TYPE_STRING, 0, 1, "Row_format"},
  {"TABLE_ROWS", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
   (MY_I_S_MAYBE_NULL | MY_I_S_UNSIGNED), "Rows"},
  {"AVG_ROW_LENGTH", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 
   (MY_I_S_MAYBE_NULL | MY_I_S_UNSIGNED), "Avg_row_length"},
  {"DATA_LENGTH", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 
   (MY_I_S_MAYBE_NULL | MY_I_S_UNSIGNED), "Data_length"},
  {"MAX_DATA_LENGTH", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
   (MY_I_S_MAYBE_NULL | MY_I_S_UNSIGNED), "Max_data_length"},
  {"INDEX_LENGTH", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 
   (MY_I_S_MAYBE_NULL | MY_I_S_UNSIGNED), "Index_length"},
  {"DATA_FREE", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
   (MY_I_S_MAYBE_NULL | MY_I_S_UNSIGNED), "Data_free"},
  {"AUTO_INCREMENT", MY_INT64_NUM_DECIMAL_DIGITS , MYSQL_TYPE_LONGLONG, 0, 
   (MY_I_S_MAYBE_NULL | MY_I_S_UNSIGNED), "Auto_increment"},
  {"CREATE_TIME", 0, MYSQL_TYPE_DATETIME, 0, 1, "Create_time"},
  {"UPDATE_TIME", 0, MYSQL_TYPE_DATETIME, 0, 1, "Update_time"},
  {"CHECK_TIME", 0, MYSQL_TYPE_DATETIME, 0, 1, "Check_time"},
  {"TABLE_COLLATION", 64, MYSQL_TYPE_STRING, 0, 1, "Collation"},
  {"CHECKSUM", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
   (MY_I_S_MAYBE_NULL | MY_I_S_UNSIGNED), "Checksum"},
  {"CREATE_OPTIONS", 255, MYSQL_TYPE_STRING, 0, 1, "Create_options"},
  {"TABLE_COMMENT", 80, MYSQL_TYPE_STRING, 0, 0, "Comment"},
  {0, 0, MYSQL_TYPE_STRING, 0, 0, 0}
};


ST_FIELD_INFO columns_fields_info[]=
{
  {"TABLE_CATALOG", FN_REFLEN, MYSQL_TYPE_STRING, 0, 1, 0},
  {"TABLE_SCHEMA", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {"TABLE_NAME", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {"COLUMN_NAME", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, "Field"},
  {"ORDINAL_POSITION", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0,
   MY_I_S_UNSIGNED, 0},
  {"COLUMN_DEFAULT", MAX_FIELD_VARCHARLENGTH, MYSQL_TYPE_STRING, 0,
   1, "Default"},
  {"IS_NULLABLE", 3, MYSQL_TYPE_STRING, 0, 0, "Null"},
  {"DATA_TYPE", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {"CHARACTER_MAXIMUM_LENGTH", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG,
   0, (MY_I_S_MAYBE_NULL | MY_I_S_UNSIGNED), 0},
  {"CHARACTER_OCTET_LENGTH", MY_INT64_NUM_DECIMAL_DIGITS , MYSQL_TYPE_LONGLONG,
   0, (MY_I_S_MAYBE_NULL | MY_I_S_UNSIGNED), 0},
  {"NUMERIC_PRECISION", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG,
   0, (MY_I_S_MAYBE_NULL | MY_I_S_UNSIGNED), 0},
  {"NUMERIC_SCALE", MY_INT64_NUM_DECIMAL_DIGITS , MYSQL_TYPE_LONGLONG,
   0, (MY_I_S_MAYBE_NULL | MY_I_S_UNSIGNED), 0},
  {"CHARACTER_SET_NAME", 64, MYSQL_TYPE_STRING, 0, 1, 0},
  {"COLLATION_NAME", 64, MYSQL_TYPE_STRING, 0, 1, "Collation"},
  {"COLUMN_TYPE", 65535, MYSQL_TYPE_STRING, 0, 0, "Type"},
  {"COLUMN_KEY", 3, MYSQL_TYPE_STRING, 0, 0, "Key"},
  {"EXTRA", 20, MYSQL_TYPE_STRING, 0, 0, "Extra"},
  {"PRIVILEGES", 80, MYSQL_TYPE_STRING, 0, 0, "Privileges"},
  {"COLUMN_COMMENT", 255, MYSQL_TYPE_STRING, 0, 0, "Comment"},
  {0, 0, MYSQL_TYPE_STRING, 0, 0, 0}
};


ST_FIELD_INFO charsets_fields_info[]=
{
  {"CHARACTER_SET_NAME", 64, MYSQL_TYPE_STRING, 0, 0, "Charset"},
  {"DEFAULT_COLLATE_NAME", 64, MYSQL_TYPE_STRING, 0, 0, "Default collation"},
  {"DESCRIPTION", 60, MYSQL_TYPE_STRING, 0, 0, "Description"},
  {"MAXLEN", 3, MYSQL_TYPE_LONGLONG, 0, 0, "Maxlen"},
  {0, 0, MYSQL_TYPE_STRING, 0, 0, 0}
};


ST_FIELD_INFO collation_fields_info[]=
{
  {"COLLATION_NAME", 64, MYSQL_TYPE_STRING, 0, 0, "Collation"},
  {"CHARACTER_SET_NAME", 64, MYSQL_TYPE_STRING, 0, 0, "Charset"},
  {"ID", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, "Id"},
  {"IS_DEFAULT", 3, MYSQL_TYPE_STRING, 0, 0, "Default"},
  {"IS_COMPILED", 3, MYSQL_TYPE_STRING, 0, 0, "Compiled"},
  {"SORTLEN", 3, MYSQL_TYPE_LONGLONG, 0, 0, "Sortlen"},
  {0, 0, MYSQL_TYPE_STRING, 0, 0, 0}
};


ST_FIELD_INFO engines_fields_info[]=
{
  {"ENGINE", 64, MYSQL_TYPE_STRING, 0, 0, "Engine"},
  {"SUPPORT", 8, MYSQL_TYPE_STRING, 0, 0, "Support"},
  {"COMMENT", 80, MYSQL_TYPE_STRING, 0, 0, "Comment"},
  {"TRANSACTIONS", 3, MYSQL_TYPE_STRING, 0, 0, "Transactions"},
  {"XA", 3, MYSQL_TYPE_STRING, 0, 0, "XA"},
  {"SAVEPOINTS", 3 ,MYSQL_TYPE_STRING, 0, 0, "Savepoints"},
  {0, 0, MYSQL_TYPE_STRING, 0, 0, 0}
};


ST_FIELD_INFO events_fields_info[]=
{
  {"EVENT_CATALOG", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 1, 0},
  {"EVENT_SCHEMA", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, "Db"},
  {"EVENT_NAME", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, "Name"},
  {"DEFINER", 77, MYSQL_TYPE_STRING, 0, 0, "Definer"},
  {"TIME_ZONE", 64, MYSQL_TYPE_STRING, 0, 0, "Time zone"},
  {"EVENT_BODY", 8, MYSQL_TYPE_STRING, 0, 0, 0},
  {"EVENT_DEFINITION", 65535, MYSQL_TYPE_STRING, 0, 0, 0},
  {"EVENT_TYPE", 9, MYSQL_TYPE_STRING, 0, 0, "Type"},
  {"EXECUTE_AT", 0, MYSQL_TYPE_DATETIME, 0, 1, "Execute at"},
  {"INTERVAL_VALUE", 256, MYSQL_TYPE_STRING, 0, 1, "Interval value"},
  {"INTERVAL_FIELD", 18, MYSQL_TYPE_STRING, 0, 1, "Interval field"},
  {"SQL_MODE", 65535, MYSQL_TYPE_STRING, 0, 0, 0},
  {"STARTS", 0, MYSQL_TYPE_DATETIME, 0, 1, "Starts"},
  {"ENDS", 0, MYSQL_TYPE_DATETIME, 0, 1, "Ends"},
  {"STATUS", 18, MYSQL_TYPE_STRING, 0, 0, "Status"},
  {"ON_COMPLETION", 12, MYSQL_TYPE_STRING, 0, 0, 0},
  {"CREATED", 0, MYSQL_TYPE_DATETIME, 0, 0, 0},
  {"LAST_ALTERED", 0, MYSQL_TYPE_DATETIME, 0, 0, 0},
  {"LAST_EXECUTED", 0, MYSQL_TYPE_DATETIME, 0, 1, 0},
  {"EVENT_COMMENT", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {"ORIGINATOR", 10, MYSQL_TYPE_LONGLONG, 0, 0, "Originator"},
  {"CHARACTER_SET_CLIENT", MY_CS_NAME_SIZE, MYSQL_TYPE_STRING, 0, 0,
   "character_set_client"},
  {"COLLATION_CONNECTION", MY_CS_NAME_SIZE, MYSQL_TYPE_STRING, 0, 0,
   "collation_connection"},
  {"DATABASE_COLLATION", MY_CS_NAME_SIZE, MYSQL_TYPE_STRING, 0, 0,
   "Database Collation"},
  {0, 0, MYSQL_TYPE_STRING, 0, 0, 0}
};



ST_FIELD_INFO coll_charset_app_fields_info[]=
{
  {"COLLATION_NAME", 64, MYSQL_TYPE_STRING, 0, 0, 0},
  {"CHARACTER_SET_NAME", 64, MYSQL_TYPE_STRING, 0, 0, 0},
  {0, 0, MYSQL_TYPE_STRING, 0, 0, 0}
};


ST_FIELD_INFO proc_fields_info[]=
{
  {"SPECIFIC_NAME", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {"ROUTINE_CATALOG", FN_REFLEN, MYSQL_TYPE_STRING, 0, 1, 0},
  {"ROUTINE_SCHEMA", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, "Db"},
  {"ROUTINE_NAME", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, "Name"},
  {"ROUTINE_TYPE", 9, MYSQL_TYPE_STRING, 0, 0, "Type"},
  {"DTD_IDENTIFIER", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 1, 0},
  {"ROUTINE_BODY", 8, MYSQL_TYPE_STRING, 0, 0, 0},
  {"ROUTINE_DEFINITION", 65535, MYSQL_TYPE_STRING, 0, 1, 0},
  {"EXTERNAL_NAME", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 1, 0},
  {"EXTERNAL_LANGUAGE", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 1, 0},
  {"PARAMETER_STYLE", 8, MYSQL_TYPE_STRING, 0, 0, 0},
  {"IS_DETERMINISTIC", 3, MYSQL_TYPE_STRING, 0, 0, 0},
  {"SQL_DATA_ACCESS", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {"SQL_PATH", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 1, 0},
  {"SECURITY_TYPE", 7, MYSQL_TYPE_STRING, 0, 0, "Security_type"},
  {"CREATED", 0, MYSQL_TYPE_DATETIME, 0, 0, "Created"},
  {"LAST_ALTERED", 0, MYSQL_TYPE_DATETIME, 0, 0, "Modified"},
  {"SQL_MODE", 65535, MYSQL_TYPE_STRING, 0, 0, 0},
  {"ROUTINE_COMMENT", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, "Comment"},
  {"DEFINER", 77, MYSQL_TYPE_STRING, 0, 0, "Definer"},
  {"CHARACTER_SET_CLIENT", MY_CS_NAME_SIZE, MYSQL_TYPE_STRING, 0, 0,
   "character_set_client"},
  {"COLLATION_CONNECTION", MY_CS_NAME_SIZE, MYSQL_TYPE_STRING, 0, 0,
   "collation_connection"},
  {"DATABASE_COLLATION", MY_CS_NAME_SIZE, MYSQL_TYPE_STRING, 0, 0,
   "Database Collation"},
  {0, 0, MYSQL_TYPE_STRING, 0, 0, 0}
};


ST_FIELD_INFO stat_fields_info[]=
{
  {"TABLE_CATALOG", FN_REFLEN, MYSQL_TYPE_STRING, 0, 1, 0},
  {"TABLE_SCHEMA", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {"TABLE_NAME", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, "Table"},
  {"NON_UNIQUE", 1, MYSQL_TYPE_LONGLONG, 0, 0, "Non_unique"},
  {"INDEX_SCHEMA", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {"INDEX_NAME", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, "Key_name"},
  {"SEQ_IN_INDEX", 2, MYSQL_TYPE_LONGLONG, 0, 0, "Seq_in_index"},
  {"COLUMN_NAME", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, "Column_name"},
  {"COLLATION", 1, MYSQL_TYPE_STRING, 0, 1, "Collation"},
  {"CARDINALITY", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 1,
   "Cardinality"},
  {"SUB_PART", 3, MYSQL_TYPE_LONGLONG, 0, 1, "Sub_part"},
  {"PACKED", 10, MYSQL_TYPE_STRING, 0, 1, "Packed"},
  {"NULLABLE", 3, MYSQL_TYPE_STRING, 0, 0, "Null"},
  {"INDEX_TYPE", 16, MYSQL_TYPE_STRING, 0, 0, "Index_type"},
  {"COMMENT", 16, MYSQL_TYPE_STRING, 0, 1, "Comment"},
  {0, 0, MYSQL_TYPE_STRING, 0, 0, 0}
};


ST_FIELD_INFO view_fields_info[]=
{
  {"TABLE_CATALOG", FN_REFLEN, MYSQL_TYPE_STRING, 0, 1, 0},
  {"TABLE_SCHEMA", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {"TABLE_NAME", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {"VIEW_DEFINITION", 65535, MYSQL_TYPE_STRING, 0, 0, 0},
  {"CHECK_OPTION", 8, MYSQL_TYPE_STRING, 0, 0, 0},
  {"IS_UPDATABLE", 3, MYSQL_TYPE_STRING, 0, 0, 0},
  {"DEFINER", 77, MYSQL_TYPE_STRING, 0, 0, 0},
  {"SECURITY_TYPE", 7, MYSQL_TYPE_STRING, 0, 0, 0},
  {"CHARACTER_SET_CLIENT", MY_CS_NAME_SIZE, MYSQL_TYPE_STRING, 0, 0, 0},
  {"COLLATION_CONNECTION", MY_CS_NAME_SIZE, MYSQL_TYPE_STRING, 0, 0, 0},
  {0, 0, MYSQL_TYPE_STRING, 0, 0, 0}
};


ST_FIELD_INFO user_privileges_fields_info[]=
{
  {"GRANTEE", 81, MYSQL_TYPE_STRING, 0, 0, 0},
  {"TABLE_CATALOG", FN_REFLEN, MYSQL_TYPE_STRING, 0, 1, 0},
  {"PRIVILEGE_TYPE", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {"IS_GRANTABLE", 3, MYSQL_TYPE_STRING, 0, 0, 0},
  {0, 0, MYSQL_TYPE_STRING, 0, 0, 0}
};


ST_FIELD_INFO schema_privileges_fields_info[]=
{
  {"GRANTEE", 81, MYSQL_TYPE_STRING, 0, 0, 0},
  {"TABLE_CATALOG", FN_REFLEN, MYSQL_TYPE_STRING, 0, 1, 0},
  {"TABLE_SCHEMA", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {"PRIVILEGE_TYPE", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {"IS_GRANTABLE", 3, MYSQL_TYPE_STRING, 0, 0, 0},
  {0, 0, MYSQL_TYPE_STRING, 0, 0, 0}
};


ST_FIELD_INFO table_privileges_fields_info[]=
{
  {"GRANTEE", 81, MYSQL_TYPE_STRING, 0, 0, 0},
  {"TABLE_CATALOG", FN_REFLEN, MYSQL_TYPE_STRING, 0, 1, 0},
  {"TABLE_SCHEMA", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {"TABLE_NAME", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {"PRIVILEGE_TYPE", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {"IS_GRANTABLE", 3, MYSQL_TYPE_STRING, 0, 0, 0},
  {0, 0, MYSQL_TYPE_STRING, 0, 0, 0}
};


ST_FIELD_INFO column_privileges_fields_info[]=
{
  {"GRANTEE", 81, MYSQL_TYPE_STRING, 0, 0, 0},
  {"TABLE_CATALOG", FN_REFLEN, MYSQL_TYPE_STRING, 0, 1, 0},
  {"TABLE_SCHEMA", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {"TABLE_NAME", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {"COLUMN_NAME", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {"PRIVILEGE_TYPE", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {"IS_GRANTABLE", 3, MYSQL_TYPE_STRING, 0, 0, 0},
  {0, 0, MYSQL_TYPE_STRING, 0, 0, 0}
};


ST_FIELD_INFO table_constraints_fields_info[]=
{
  {"CONSTRAINT_CATALOG", FN_REFLEN, MYSQL_TYPE_STRING, 0, 1, 0},
  {"CONSTRAINT_SCHEMA", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {"CONSTRAINT_NAME", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {"TABLE_SCHEMA", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {"TABLE_NAME", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {"CONSTRAINT_TYPE", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {0, 0, MYSQL_TYPE_STRING, 0, 0, 0}
};


ST_FIELD_INFO key_column_usage_fields_info[]=
{
  {"CONSTRAINT_CATALOG", FN_REFLEN, MYSQL_TYPE_STRING, 0, 1, 0},
  {"CONSTRAINT_SCHEMA", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {"CONSTRAINT_NAME", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {"TABLE_CATALOG", FN_REFLEN, MYSQL_TYPE_STRING, 0, 1, 0},
  {"TABLE_SCHEMA", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {"TABLE_NAME", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {"COLUMN_NAME", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {"ORDINAL_POSITION", 10 ,MYSQL_TYPE_LONGLONG, 0, 0, 0},
  {"POSITION_IN_UNIQUE_CONSTRAINT", 10 ,MYSQL_TYPE_LONGLONG, 0, 1, 0},
  {"REFERENCED_TABLE_SCHEMA", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 1, 0},
  {"REFERENCED_TABLE_NAME", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 1, 0},
  {"REFERENCED_COLUMN_NAME", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 1, 0},
  {0, 0, MYSQL_TYPE_STRING, 0, 0, 0}
};


ST_FIELD_INFO table_names_fields_info[]=
{
  {"TABLE_CATALOG", FN_REFLEN, MYSQL_TYPE_STRING, 0, 1, 0},
  {"TABLE_SCHEMA",NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {"TABLE_NAME", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, "Tables_in_"},
  {"TABLE_TYPE", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, "Table_type"},
  {0, 0, MYSQL_TYPE_STRING, 0, 0, 0}
};


ST_FIELD_INFO open_tables_fields_info[]=
{
  {"Database", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, "Database"},
  {"Table",NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, "Table"},
  {"In_use", 1, MYSQL_TYPE_LONGLONG, 0, 0, "In_use"},
  {"Name_locked", 4, MYSQL_TYPE_LONGLONG, 0, 0, "Name_locked"},
  {0, 0, MYSQL_TYPE_STRING, 0, 0, 0}
};


ST_FIELD_INFO triggers_fields_info[]=
{
  {"TRIGGER_CATALOG", FN_REFLEN, MYSQL_TYPE_STRING, 0, 1, 0},
  {"TRIGGER_SCHEMA",NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {"TRIGGER_NAME", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, "Trigger"},
  {"EVENT_MANIPULATION", 6, MYSQL_TYPE_STRING, 0, 0, "Event"},
  {"EVENT_OBJECT_CATALOG", FN_REFLEN, MYSQL_TYPE_STRING, 0, 1, 0},
  {"EVENT_OBJECT_SCHEMA",NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {"EVENT_OBJECT_TABLE", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, "Table"},
  {"ACTION_ORDER", 4, MYSQL_TYPE_LONGLONG, 0, 0, 0},
  {"ACTION_CONDITION", 65535, MYSQL_TYPE_STRING, 0, 1, 0},
  {"ACTION_STATEMENT", 65535, MYSQL_TYPE_STRING, 0, 0, "Statement"},
  {"ACTION_ORIENTATION", 9, MYSQL_TYPE_STRING, 0, 0, 0},
  {"ACTION_TIMING", 6, MYSQL_TYPE_STRING, 0, 0, "Timing"},
  {"ACTION_REFERENCE_OLD_TABLE", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 1, 0},
  {"ACTION_REFERENCE_NEW_TABLE", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 1, 0},
  {"ACTION_REFERENCE_OLD_ROW", 3, MYSQL_TYPE_STRING, 0, 0, 0},
  {"ACTION_REFERENCE_NEW_ROW", 3, MYSQL_TYPE_STRING, 0, 0, 0},
  {"CREATED", 0, MYSQL_TYPE_DATETIME, 0, 1, "Created"},
  {"SQL_MODE", 65535, MYSQL_TYPE_STRING, 0, 0, "sql_mode"},
  {"DEFINER", 65535, MYSQL_TYPE_STRING, 0, 0, "Definer"},
  {"CHARACTER_SET_CLIENT", MY_CS_NAME_SIZE, MYSQL_TYPE_STRING, 0, 0,
   "character_set_client"},
  {"COLLATION_CONNECTION", MY_CS_NAME_SIZE, MYSQL_TYPE_STRING, 0, 0,
   "collation_connection"},
  {"DATABASE_COLLATION", MY_CS_NAME_SIZE, MYSQL_TYPE_STRING, 0, 0,
   "Database Collation"},
  {0, 0, MYSQL_TYPE_STRING, 0, 0, 0}
};


ST_FIELD_INFO partitions_fields_info[]=
{
  {"TABLE_CATALOG", FN_REFLEN, MYSQL_TYPE_STRING, 0, 1, 0},
  {"TABLE_SCHEMA",NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {"TABLE_NAME", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {"PARTITION_NAME", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 1, 0},
  {"SUBPARTITION_NAME", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 1, 0},
  {"PARTITION_ORDINAL_POSITION", 21 , MYSQL_TYPE_LONGLONG, 0,
   (MY_I_S_MAYBE_NULL | MY_I_S_UNSIGNED), 0},
  {"SUBPARTITION_ORDINAL_POSITION", 21 , MYSQL_TYPE_LONGLONG, 0,
   (MY_I_S_MAYBE_NULL | MY_I_S_UNSIGNED), 0},
  {"PARTITION_METHOD", 12, MYSQL_TYPE_STRING, 0, 1, 0},
  {"SUBPARTITION_METHOD", 12, MYSQL_TYPE_STRING, 0, 1, 0},
  {"PARTITION_EXPRESSION", 65535, MYSQL_TYPE_STRING, 0, 1, 0},
  {"SUBPARTITION_EXPRESSION", 65535, MYSQL_TYPE_STRING, 0, 1, 0},
  {"PARTITION_DESCRIPTION", 65535, MYSQL_TYPE_STRING, 0, 1, 0},
  {"TABLE_ROWS", 21 , MYSQL_TYPE_LONGLONG, 0, MY_I_S_UNSIGNED, 0},
  {"AVG_ROW_LENGTH", 21 , MYSQL_TYPE_LONGLONG, 0, MY_I_S_UNSIGNED, 0},
  {"DATA_LENGTH", 21 , MYSQL_TYPE_LONGLONG, 0, MY_I_S_UNSIGNED, 0},
  {"MAX_DATA_LENGTH", 21 , MYSQL_TYPE_LONGLONG, 0,
   (MY_I_S_MAYBE_NULL | MY_I_S_UNSIGNED), 0},
  {"INDEX_LENGTH", 21 , MYSQL_TYPE_LONGLONG, 0, MY_I_S_UNSIGNED, 0},
  {"DATA_FREE", 21 , MYSQL_TYPE_LONGLONG, 0, MY_I_S_UNSIGNED, 0},
  {"CREATE_TIME", 0, MYSQL_TYPE_DATETIME, 0, 1, 0},
  {"UPDATE_TIME", 0, MYSQL_TYPE_DATETIME, 0, 1, 0},
  {"CHECK_TIME", 0, MYSQL_TYPE_DATETIME, 0, 1, 0},
  {"CHECKSUM", 21 , MYSQL_TYPE_LONGLONG, 0,
   (MY_I_S_MAYBE_NULL | MY_I_S_UNSIGNED), 0},
  {"PARTITION_COMMENT", 80, MYSQL_TYPE_STRING, 0, 0, 0},
  {"NODEGROUP", 12 , MYSQL_TYPE_STRING, 0, 0, 0},
  {"TABLESPACE_NAME", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 1, 0},
  {0, 0, MYSQL_TYPE_STRING, 0, 0, 0}
};


ST_FIELD_INFO variables_fields_info[]=
{
  {"VARIABLE_NAME", 64, MYSQL_TYPE_STRING, 0, 0, "Variable_name"},
  {"VARIABLE_VALUE", 20480, MYSQL_TYPE_STRING, 0, 1, "Value"},
  {0, 0, MYSQL_TYPE_STRING, 0, 0, 0}
};


ST_FIELD_INFO processlist_fields_info[]=
{
  {"ID", 4, MYSQL_TYPE_LONGLONG, 0, 0, "Id"},
  {"USER", 16, MYSQL_TYPE_STRING, 0, 0, "User"},
  {"HOST", LIST_PROCESS_HOST_LEN,  MYSQL_TYPE_STRING, 0, 0, "Host"},
  {"DB", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 1, "Db"},
  {"COMMAND", 16, MYSQL_TYPE_STRING, 0, 0, "Command"},
  {"TIME", 7, MYSQL_TYPE_LONGLONG, 0, 0, "Time"},
  {"STATE", 64, MYSQL_TYPE_STRING, 0, 1, "State"},
  {"INFO", PROCESS_LIST_INFO_WIDTH, MYSQL_TYPE_STRING, 0, 1, "Info"},
  {0, 0, MYSQL_TYPE_STRING, 0, 0, 0}
};


ST_FIELD_INFO plugin_fields_info[]=
{
  {"PLUGIN_NAME", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, "Name"},
  {"PLUGIN_VERSION", 20, MYSQL_TYPE_STRING, 0, 0, 0},
  {"PLUGIN_STATUS", 10, MYSQL_TYPE_STRING, 0, 0, "Status"},
  {"PLUGIN_TYPE", 80, MYSQL_TYPE_STRING, 0, 0, "Type"},
  {"PLUGIN_TYPE_VERSION", 20, MYSQL_TYPE_STRING, 0, 0, 0},
  {"PLUGIN_LIBRARY", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 1, "Library"},
  {"PLUGIN_LIBRARY_VERSION", 20, MYSQL_TYPE_STRING, 0, 1, 0},
  {"PLUGIN_AUTHOR", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 1, 0},
  {"PLUGIN_DESCRIPTION", 65535, MYSQL_TYPE_STRING, 0, 1, 0},
  {"PLUGIN_LICENSE", 80, MYSQL_TYPE_STRING, 0, 1, "License"},
  {0, 0, MYSQL_TYPE_STRING, 0, 0, 0}
};

ST_FIELD_INFO files_fields_info[]=
{
  {"FILE_ID", 4, MYSQL_TYPE_LONGLONG, 0, 0, 0},
  {"FILE_NAME", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 1, 0},
  {"FILE_TYPE", 20, MYSQL_TYPE_STRING, 0, 0, 0},
  {"TABLESPACE_NAME", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 1, 0},
  {"TABLE_CATALOG", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 1, 0},
  {"TABLE_SCHEMA", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 1, 0},
  {"TABLE_NAME", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 1, 0},
  {"LOGFILE_GROUP_NAME", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 1, 0},
  {"LOGFILE_GROUP_NUMBER", 4, MYSQL_TYPE_LONGLONG, 0, 1, 0},
  {"ENGINE", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {"FULLTEXT_KEYS", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 1, 0},
  {"DELETED_ROWS", 4, MYSQL_TYPE_LONGLONG, 0, 1, 0},
  {"UPDATE_COUNT", 4, MYSQL_TYPE_LONGLONG, 0, 1, 0},
  {"FREE_EXTENTS", 4, MYSQL_TYPE_LONGLONG, 0, 1, 0},
  {"TOTAL_EXTENTS", 4, MYSQL_TYPE_LONGLONG, 0, 1, 0},
  {"EXTENT_SIZE", 4, MYSQL_TYPE_LONGLONG, 0, 0, 0},
  {"INITIAL_SIZE", 21, MYSQL_TYPE_LONGLONG, 0,
   (MY_I_S_MAYBE_NULL | MY_I_S_UNSIGNED), 0},
  {"MAXIMUM_SIZE", 21, MYSQL_TYPE_LONGLONG, 0, 
   (MY_I_S_MAYBE_NULL | MY_I_S_UNSIGNED), 0},
  {"AUTOEXTEND_SIZE", 21, MYSQL_TYPE_LONGLONG, 0, 
   (MY_I_S_MAYBE_NULL | MY_I_S_UNSIGNED), 0},
  {"CREATION_TIME", 0, MYSQL_TYPE_DATETIME, 0, 1, 0},
  {"LAST_UPDATE_TIME", 0, MYSQL_TYPE_DATETIME, 0, 1, 0},
  {"LAST_ACCESS_TIME", 0, MYSQL_TYPE_DATETIME, 0, 1, 0},
  {"RECOVER_TIME", 4, MYSQL_TYPE_LONGLONG, 0, 1, 0},
  {"TRANSACTION_COUNTER", 4, MYSQL_TYPE_LONGLONG, 0, 1, 0},
  {"VERSION", 21 , MYSQL_TYPE_LONGLONG, 0,
   (MY_I_S_MAYBE_NULL | MY_I_S_UNSIGNED), "Version"},
  {"ROW_FORMAT", 10, MYSQL_TYPE_STRING, 0, 1, "Row_format"},
  {"TABLE_ROWS", 21 , MYSQL_TYPE_LONGLONG, 0,
   (MY_I_S_MAYBE_NULL | MY_I_S_UNSIGNED), "Rows"},
  {"AVG_ROW_LENGTH", 21 , MYSQL_TYPE_LONGLONG, 0, 
   (MY_I_S_MAYBE_NULL | MY_I_S_UNSIGNED), "Avg_row_length"},
  {"DATA_LENGTH", 21 , MYSQL_TYPE_LONGLONG, 0, 
   (MY_I_S_MAYBE_NULL | MY_I_S_UNSIGNED), "Data_length"},
  {"MAX_DATA_LENGTH", 21 , MYSQL_TYPE_LONGLONG, 0, 
   (MY_I_S_MAYBE_NULL | MY_I_S_UNSIGNED), "Max_data_length"},
  {"INDEX_LENGTH", 21 , MYSQL_TYPE_LONGLONG, 0, 
   (MY_I_S_MAYBE_NULL | MY_I_S_UNSIGNED), "Index_length"},
  {"DATA_FREE", 21 , MYSQL_TYPE_LONGLONG, 0, 
   (MY_I_S_MAYBE_NULL | MY_I_S_UNSIGNED), "Data_free"},
  {"CREATE_TIME", 0, MYSQL_TYPE_DATETIME, 0, 1, "Create_time"},
  {"UPDATE_TIME", 0, MYSQL_TYPE_DATETIME, 0, 1, "Update_time"},
  {"CHECK_TIME", 0, MYSQL_TYPE_DATETIME, 0, 1, "Check_time"},
  {"CHECKSUM", 21 , MYSQL_TYPE_LONGLONG, 0, 
   (MY_I_S_MAYBE_NULL | MY_I_S_UNSIGNED), "Checksum"},
  {"STATUS", 20, MYSQL_TYPE_STRING, 0, 0, 0},
  {"EXTRA", 255, MYSQL_TYPE_STRING, 0, 1, 0},
  {0, 0, MYSQL_TYPE_STRING, 0, 0, 0}
};

void init_fill_schema_files_row(TABLE* table)
{
  int i;
  for(i=0; files_fields_info[i].field_name!=NULL; i++)
    table->field[i]->set_null();

  table->field[IS_FILES_STATUS]->set_notnull();
  table->field[IS_FILES_STATUS]->store("NORMAL", 6, system_charset_info);
}

ST_FIELD_INFO referential_constraints_fields_info[]=
{
  {"CONSTRAINT_CATALOG", FN_REFLEN, MYSQL_TYPE_STRING, 0, 1, 0},
  {"CONSTRAINT_SCHEMA", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {"CONSTRAINT_NAME", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {"UNIQUE_CONSTRAINT_CATALOG", FN_REFLEN, MYSQL_TYPE_STRING, 0, 1, 0},
  {"UNIQUE_CONSTRAINT_SCHEMA", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {"UNIQUE_CONSTRAINT_NAME", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {"MATCH_OPTION", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {"UPDATE_RULE", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {"DELETE_RULE", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {"TABLE_NAME", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {"REFERENCED_TABLE_NAME", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, 0},
  {0, 0, MYSQL_TYPE_STRING, 0, 0, 0}
};


/*
  Description of ST_FIELD_INFO in table.h

  Make sure that the order of schema_tables and enum_schema_tables are the same.

*/

ST_SCHEMA_TABLE schema_tables[]=
{
  {"CHARACTER_SETS", charsets_fields_info, create_schema_table, 
   fill_schema_charsets, make_character_sets_old_format, 0, -1, -1, 0},
  {"COLLATIONS", collation_fields_info, create_schema_table, 
   fill_schema_collation, make_old_format, 0, -1, -1, 0},
  {"COLLATION_CHARACTER_SET_APPLICABILITY", coll_charset_app_fields_info,
   create_schema_table, fill_schema_coll_charset_app, 0, 0, -1, -1, 0},
  {"COLUMNS", columns_fields_info, create_schema_table, 
   get_all_tables, make_columns_old_format, get_schema_column_record, 1, 2, 0},
  {"COLUMN_PRIVILEGES", column_privileges_fields_info, create_schema_table,
    fill_schema_column_privileges, 0, 0, -1, -1, 0},
  {"ENGINES", engines_fields_info, create_schema_table,
   fill_schema_engines, make_old_format, 0, -1, -1, 0},
  {"EVENTS", events_fields_info, create_schema_table,
   Events::fill_schema_events, make_old_format, 0, -1, -1, 0},
  {"FILES", files_fields_info, create_schema_table,
   fill_schema_files, 0, 0, -1, -1, 0},
  {"GLOBAL_STATUS", variables_fields_info, create_schema_table,
   fill_status, make_old_format, 0, -1, -1, 0},
  {"GLOBAL_VARIABLES", variables_fields_info, create_schema_table,
   fill_variables, make_old_format, 0, -1, -1, 0},
  {"KEY_COLUMN_USAGE", key_column_usage_fields_info, create_schema_table,
    get_all_tables, 0, get_schema_key_column_usage_record, 4, 5, 0},
  {"OPEN_TABLES", open_tables_fields_info, create_schema_table,
   fill_open_tables, make_old_format, 0, -1, -1, 1},
  {"PARTITIONS", partitions_fields_info, create_schema_table,
   get_all_tables, 0, get_schema_partitions_record, 1, 2, 0},
  {"PLUGINS", plugin_fields_info, create_schema_table,
    fill_plugins, make_old_format, 0, -1, -1, 0},
  {"PROCESSLIST", processlist_fields_info, create_schema_table,
    fill_schema_processlist, make_old_format, 0, -1, -1, 0},
  {"REFERENTIAL_CONSTRAINTS", referential_constraints_fields_info,
   create_schema_table, get_all_tables, 0, get_referential_constraints_record,
   1, 9, 0},
  {"ROUTINES", proc_fields_info, create_schema_table, 
    fill_schema_proc, make_proc_old_format, 0, -1, -1, 0},
  {"SCHEMATA", schema_fields_info, create_schema_table,
   fill_schema_shemata, make_schemata_old_format, 0, 1, -1, 0},
  {"SCHEMA_PRIVILEGES", schema_privileges_fields_info, create_schema_table,
    fill_schema_schema_privileges, 0, 0, -1, -1, 0},
  {"SESSION_STATUS", variables_fields_info, create_schema_table,
    fill_status, make_old_format, 0, -1, -1, 0},
  {"SESSION_VARIABLES", variables_fields_info, create_schema_table,
    fill_variables, make_old_format, 0, -1, -1, 0},
  {"STATISTICS", stat_fields_info, create_schema_table, 
    get_all_tables, make_old_format, get_schema_stat_record, 1, 2, 0},
  {"STATUS", variables_fields_info, create_schema_table, fill_status, 
   make_old_format, 0, -1, -1, 1},
  {"TABLES", tables_fields_info, create_schema_table, 
   get_all_tables, make_old_format, get_schema_tables_record, 1, 2, 0},
  {"TABLE_CONSTRAINTS", table_constraints_fields_info, create_schema_table,
    get_all_tables, 0, get_schema_constraints_record, 3, 4, 0},
  {"TABLE_NAMES", table_names_fields_info, create_schema_table,
   get_all_tables, make_table_names_old_format, 0, 1, 2, 1},
  {"TABLE_PRIVILEGES", table_privileges_fields_info, create_schema_table,
    fill_schema_table_privileges, 0, 0, -1, -1, 0},
  {"TRIGGERS", triggers_fields_info, create_schema_table,
   get_all_tables, make_old_format, get_schema_triggers_record, 5, 6, 0},
  {"USER_PRIVILEGES", user_privileges_fields_info, create_schema_table, 
    fill_schema_user_privileges, 0, 0, -1, -1, 0},
  {"VARIABLES", variables_fields_info, create_schema_table, fill_variables,
   make_old_format, 0, -1, -1, 1},
  {"VIEWS", view_fields_info, create_schema_table, 
    get_all_tables, 0, get_schema_views_record, 1, 2, 0},
  {0, 0, 0, 0, 0, 0, 0, 0, 0}
};


#ifdef HAVE_EXPLICIT_TEMPLATE_INSTANTIATION
template class List_iterator_fast<char>;
template class List<char>;
#endif

int initialize_schema_table(st_plugin_int *plugin)
{
  ST_SCHEMA_TABLE *schema_table;
  DBUG_ENTER("initialize_schema_table");

  if (!(schema_table= (ST_SCHEMA_TABLE *)my_malloc(sizeof(ST_SCHEMA_TABLE),
                                MYF(MY_WME | MY_ZEROFILL))))
      DBUG_RETURN(1);
  /* Historical Requirement */
  plugin->data= schema_table; // shortcut for the future
  if (plugin->plugin->init)
  {
    schema_table->create_table= create_schema_table;
    schema_table->old_format= make_old_format;
    schema_table->idx_field1= -1, 
    schema_table->idx_field2= -1; 

    if (plugin->plugin->init(schema_table))
    {
      sql_print_error("Plugin '%s' init function returned error.",
                      plugin->name.str);
      goto err;
    }
    schema_table->table_name= plugin->name.str;
  }

  DBUG_RETURN(0);
err:
  my_free(schema_table, MYF(0));
  DBUG_RETURN(1);
}

int finalize_schema_table(st_plugin_int *plugin)
{
  ST_SCHEMA_TABLE *schema_table= (ST_SCHEMA_TABLE *)plugin->data;
  DBUG_ENTER("finalize_schema_table");

  if (schema_table && plugin->plugin->deinit)
  {
    DBUG_PRINT("info", ("Deinitializing plugin: '%s'", plugin->name.str));
    if (plugin->plugin->deinit(NULL))
    {
      DBUG_PRINT("warning", ("Plugin '%s' deinit function returned error.",
                             plugin->name.str));
    }
    my_free(schema_table, MYF(0));
  }
  DBUG_RETURN(0);
}


/**
  Output trigger information (SHOW CREATE TRIGGER) to the client.

  @param thd          Thread context.
  @param triggers     List of triggers for the table.
  @param trigger_idx  Index of the trigger to dump.

  @return Operation status
    @retval TRUE Error.
    @retval FALSE Success.
*/

static bool show_create_trigger_impl(THD *thd,
                                     Table_triggers_list *triggers,
                                     int trigger_idx)
{
  int ret_code;

  Protocol *p= thd->protocol;
  List<Item> fields;

  LEX_STRING trg_name;
  ulonglong trg_sql_mode;
  LEX_STRING trg_sql_mode_str;
  LEX_STRING trg_sql_original_stmt;
  LEX_STRING trg_client_cs_name;
  LEX_STRING trg_connection_cl_name;
  LEX_STRING trg_db_cl_name;

  /*
    TODO: Check privileges here. This functionality will be added by
    implementation of the following WL items:
      - WL#2227: New privileges for new objects
      - WL#3482: Protect SHOW CREATE PROCEDURE | FUNCTION | VIEW | TRIGGER
        properly

    SHOW TRIGGERS and I_S.TRIGGERS will be affected too.
  */

  /* Prepare trigger "object". */

  triggers->get_trigger_info(thd,
                             trigger_idx,
                             &trg_name,
                             &trg_sql_mode,
                             &trg_sql_original_stmt,
                             &trg_client_cs_name,
                             &trg_connection_cl_name,
                             &trg_db_cl_name);

  sys_var_thd_sql_mode::symbolic_mode_representation(thd,
                                                     trg_sql_mode,
                                                     &trg_sql_mode_str);

  /* Send header. */

  fields.push_back(new Item_empty_string("Trigger", NAME_LEN));
  fields.push_back(new Item_empty_string("sql_mode", trg_sql_mode_str.length));

  {
    /*
      NOTE: SQL statement field must be not less than 1024 in order not to
      confuse old clients.
    */

    Item_empty_string *stmt_fld=
      new Item_empty_string("SQL Original Statement",
                            max(trg_sql_original_stmt.length, 1024));

    stmt_fld->maybe_null= TRUE;

    fields.push_back(stmt_fld);
  }

  fields.push_back(new Item_empty_string("character_set_client",
                                         MY_CS_NAME_SIZE));

  fields.push_back(new Item_empty_string("collation_connection",
                                         MY_CS_NAME_SIZE));

  fields.push_back(new Item_empty_string("Database Collation",
                                         MY_CS_NAME_SIZE));

  if (p->send_fields(&fields, Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF))
    return TRUE;

  /* Send data. */

  p->prepare_for_resend();

  p->store(trg_name.str,
           trg_name.length,
           system_charset_info);

  p->store(trg_sql_mode_str.str,
           trg_sql_mode_str.length,
           system_charset_info);

  p->store(trg_sql_original_stmt.str,
           trg_sql_original_stmt.length,
           &my_charset_bin);

  p->store(trg_client_cs_name.str,
           trg_client_cs_name.length,
           system_charset_info);

  p->store(trg_connection_cl_name.str,
           trg_connection_cl_name.length,
           system_charset_info);

  p->store(trg_db_cl_name.str,
           trg_db_cl_name.length,
           system_charset_info);

  ret_code= p->write();

  if (!ret_code)
    send_eof(thd);

  return ret_code != 0;
}


/**
  Read TRN and TRG files to obtain base table name for the specified
  trigger name and construct TABE_LIST object for the base table.

  @param thd      Thread context.
  @param trg_name Trigger name.

  @return TABLE_LIST object corresponding to the base table.

  TODO: This function is a copy&paste from add_table_to_list() and
  sp_add_to_query_tables(). The problem is that in order to be compatible
  with Stored Programs (Prepared Statements), we should not touch thd->lex.
  The "source" functions also add created TABLE_LIST object to the
  thd->lex->query_tables.

  The plan to eliminate this copy&paste is to:

    - get rid of sp_add_to_query_tables() and use Lex::add_table_to_list().
      Only add_table_to_list() must be used to add tables from the parser
      into Lex::query_tables list.

    - do not update Lex::query_tables in add_table_to_list().
*/

static TABLE_LIST *get_trigger_table_impl(
  THD *thd,
  const sp_name *trg_name)
{
  char trn_path_buff[FN_REFLEN];

  LEX_STRING trn_path= { trn_path_buff, 0 };
  LEX_STRING tbl_name;

  build_trn_path(thd, trg_name, &trn_path);

  if (check_trn_exists(&trn_path))
  {
    my_error(ER_TRG_DOES_NOT_EXIST, MYF(0));
    return NULL;
  }

  if (load_table_name_for_trigger(thd, trg_name, &trn_path, &tbl_name))
    return NULL;

  /* We need to reset statement table list to be PS/SP friendly. */

  TABLE_LIST *table;

  if (!(table= (TABLE_LIST *)thd->calloc(sizeof(TABLE_LIST))))
  {
    my_error(ER_OUTOFMEMORY, MYF(0), sizeof(TABLE_LIST));
    return NULL;
  }

  table->db_length= trg_name->m_db.length;
  table->db= thd->strmake(trg_name->m_db.str, trg_name->m_db.length);

  table->table_name_length= tbl_name.length;
  table->table_name= thd->strmake(tbl_name.str, tbl_name.length);

  table->alias= thd->strmake(tbl_name.str, tbl_name.length);

  table->lock_type= TL_IGNORE;
  table->cacheable_table= 0;

  return table;
}

/**
  Read TRN and TRG files to obtain base table name for the specified
  trigger name and construct TABE_LIST object for the base table. Acquire
  LOCK_open when doing this.

  @param thd      Thread context.
  @param trg_name Trigger name.

  @return TABLE_LIST object corresponding to the base table.
*/

static TABLE_LIST *get_trigger_table(THD *thd, const sp_name *trg_name)
{
  /* Acquire LOCK_open (stop the server). */

  pthread_mutex_lock(&LOCK_open);

  /*
    Load base table name from the TRN-file and create TABLE_LIST object.
  */

  TABLE_LIST *lst= get_trigger_table_impl(thd, trg_name);

  /* Release LOCK_open (continue the server). */

  pthread_mutex_unlock(&LOCK_open);

  /* That's it. */

  return lst;
}


/**
  SHOW CREATE TRIGGER high-level implementation.

  @param thd      Thread context.
  @param trg_name Trigger name.

  @return Operation status
    @retval TRUE Error.
    @retval FALSE Success.
*/

bool show_create_trigger(THD *thd, const sp_name *trg_name)
{
  TABLE_LIST *lst= get_trigger_table(thd, trg_name);

  /*
    Open the table by name in order to load Table_triggers_list object.

    NOTE: there is race condition here -- the table can be dropped after
    LOCK_open is released. It will be fixed later by introducing
    acquire-shared-table-name-lock functionality.
  */

  uint num_tables; /* NOTE: unused, only to pass to open_tables(). */

  if (open_tables(thd, &lst, &num_tables, 0))
  {
    my_error(ER_TRG_CANT_OPEN_TABLE, MYF(0),
             (const char *) trg_name->m_db.str,
             (const char *) lst->table_name);

    return TRUE;

    /* Perform closing actions and return error status. */
  }

  DBUG_ASSERT(num_tables == 1);

  Table_triggers_list *triggers= lst->table->triggers;

  if (!triggers)
  {
    my_error(ER_TRG_DOES_NOT_EXIST, MYF(0));
    return TRUE;
  }

  int trigger_idx= triggers->find_trigger_by_name(&trg_name->m_name);

  if (trigger_idx < 0)
  {
    my_error(ER_TRG_CORRUPTED_FILE, MYF(0),
             (const char *) trg_name->m_db.str,
             (const char *) lst->table_name);

    return TRUE;
  }

  return show_create_trigger_impl(thd, triggers, trigger_idx);

  /*
    NOTE: if show_create_trigger_impl() failed, that means we could not
    send data to the client. In this case we simply raise the error
    status and client connection will be closed.
  */
}
