#ifndef _REISER_FS_I
#define _REISER_FS_I

#define REISERFS_N_BLOCKS 10

struct reiserfs_inode_info {
  struct pipe_inode_info reserved;
  __u32 i_key [4];
  __u32 i_first_direct_byte;

  int i_data_length;
  __u32 i_data [REISERFS_N_BLOCKS];
  int i_is_being_converted;
  int i_read_sync_counter;
  int i_pack_on_close ;
  int i_transaction_index ;
  int i_transaction_id ;
};


#endif
