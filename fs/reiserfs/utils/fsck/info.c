
/*
 * Copyright 1996-1999 Hans Reiser
 */
#include "fsck.h"

struct fsck_stat g_fsck_info = {0, };


void add_event (int event)
{
  switch (event) {
    /* tree building (pass 1 and 2) info */
  case GOOD_LEAVES:
    g_fsck_info.fs_good_leaves ++; break;
  case UNINSERTABLE_LEAVES:
    g_fsck_info.fs_uninsertable_leaves ++; break;
  case REWRITTEN_FILES:
    g_fsck_info.fs_rewritten_files ++; break;
  case LEAVES_USED_BY_INDIRECT_ITEMS:
    g_fsck_info.fs_leaves_used_by_indirect_items ++; break;
  case UNFM_OVERWRITING_UNFM:
    g_fsck_info.fs_unfm_overwriting_unfm ++; break;
  case INDIRECT_TO_DIRECT:
    g_fsck_info.fs_indirect_to_direct ++; break;

    /* pass 3 info (semantic) */
  case FIXED_SIZE_DIRECTORIES:
    g_fsck_info.fs_fixed_size_directories ++; break;
  case INCORRECT_REGULAR_FILES:
    /* file has incorrect sequence of items (incorrect items are truncated) */
    g_fsck_info.fs_incorrect_regular_files ++; break;
  case FIXED_SIZE_FILES:
    g_fsck_info.fs_fixed_size_files ++; break;

    /* pass 4 info */
  case UNACCESSED_ITEMS:
    g_fsck_info.fs_unaccessed_items ++; break;
  case FIXED_RIGHT_DELIM_KEY:
    g_fsck_info.fs_fixed_right_delim_key ++; break;

    /* file system info */
  case STAT_DATA_ITEMS:
    g_fsck_info.fs_stat_data_items ++; break;
  case REGULAR_FILES:
    g_fsck_info.fs_regular_files ++; break;
  case DIRECTORIES:
    g_fsck_info.fs_directories ++; break;
  case SYMLINKS:
    g_fsck_info.fs_symlinks ++; break;
  case OTHERS:
    g_fsck_info.fs_others ++; break;
  }
}


int get_event (int event)
{
  switch (event) {
  case GOOD_LEAVES:
    return g_fsck_info.fs_good_leaves;
  case UNINSERTABLE_LEAVES:
    return g_fsck_info.fs_uninsertable_leaves;
  case REGULAR_FILES:
    return g_fsck_info.fs_regular_files;
  case INCORRECT_REGULAR_FILES:
    return g_fsck_info.fs_incorrect_regular_files;
  case DIRECTORIES:
    return g_fsck_info.fs_directories;
  case FIXED_SIZE_DIRECTORIES:
    return g_fsck_info.fs_fixed_size_directories;
  case STAT_DATA_ITEMS:
    return g_fsck_info.fs_stat_data_items;
  }
  return 0;
}

/* outputs information about inconsistencies */
void output_information ()
{
  FILE * fp;
  char buf[160];
/*
  if (opt_verbose == 0)
    return;
*/
  fp = stderr;

/*  time (&t);
  fputs ("**** This is reiserfsck log file: created ", fp); fputs (ctime (&t), fp); fputs ("\n", fp);*/
  fputs ("Building S+ tree info\n", fp);
  sprintf (buf, "\tGood leaves: %d\n", g_fsck_info.fs_good_leaves); fputs (buf, fp);
  sprintf (buf, "\tBad leaves: %d\n", g_fsck_info.fs_uninsertable_leaves); fputs (buf, fp);
  sprintf (buf, "\tRewritten files: %d\n", g_fsck_info.fs_rewritten_files); fputs (buf, fp);
  sprintf (buf, "\tLeaves pointed by indirect item: %d\n", g_fsck_info.fs_leaves_used_by_indirect_items); fputs (buf, fp);
  sprintf (buf, "\tUnformatted nodes overwritten by direct items\nand then by other unformatted node: %d\n",
	   g_fsck_info.fs_unfm_overwriting_unfm); fputs (buf, fp);
  sprintf (buf, "\tIndirect_to_direct conversions: %d\n", g_fsck_info.fs_indirect_to_direct); fputs (buf, fp);

  fputs ("Semantic pass info\n", fp);
  sprintf (buf, "\tFiles with fixed size: %d\n", g_fsck_info.fs_fixed_size_files); fputs (buf, fp);
  sprintf (buf, "\tDirectories with fixed size: %d\n", g_fsck_info.fs_fixed_size_directories); fputs (buf, fp);
  sprintf (buf, "\tEntries pointing to nowhere (deleted): %d\n", g_fsck_info.fs_deleted_entries); fputs (buf, fp);

  fputs ("Pass 4 info\n", fp);
  sprintf (buf, "\tUnaccessed items found (and deleted): %d\n", g_fsck_info.fs_unaccessed_items); fputs (buf, fp);
  sprintf (buf, "\tFixed right delimiting keys: %d\n", g_fsck_info.fs_fixed_right_delim_key); fputs (buf, fp);
  sprintf (buf, "\tStat datas: %d\n", g_fsck_info.fs_stat_data_items); fputs (buf, fp);


  fputs ("File system info\n", fp);
  sprintf (buf, "\tFiles found: %d\n", g_fsck_info.fs_regular_files); fputs (buf, fp);
  sprintf (buf, "\tDirectories found: %d\n", g_fsck_info.fs_directories); fputs (buf, fp);
  sprintf (buf, "\tSymlinks found: %d\n", g_fsck_info.fs_symlinks); fputs (buf, fp);
  sprintf (buf, "\tOthers: %d\n", g_fsck_info.fs_others); fputs (buf, fp);

  /*fclose (fp);*/
}




