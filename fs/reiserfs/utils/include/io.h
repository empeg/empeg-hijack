struct buffer_head {
  unsigned long b_blocknr;
  unsigned short b_dev;
  unsigned long b_size;
  char * b_data;
  unsigned long b_state;
  unsigned int b_count;
  unsigned int b_list ;
  void (*b_end_io)(struct buffer_head *bh, int uptodate);

  struct buffer_head * b_next;
  struct buffer_head * b_prev;
  struct buffer_head * b_hash_next;
  struct buffer_head * b_hash_prev;
};

#define BH_Uptodate	0
#define BH_Dirty	1
#define BH_Lock		2
#define BUF_DIRTY	1


#define buffer_uptodate(bh) test_bit(BH_Uptodate, &(bh)->b_state)
#define buffer_dirty(bh) test_bit(BH_Dirty, &(bh)->b_state)
#define buffer_locked(bh) test_bit(BH_Lock, &(bh)->b_state)
#define buffer_clean(bh) !test_bit(BH_Dirty, &(bh)->b_state)
#define mark_buffer_dirty(bh,i) set_bit(BH_Dirty, &(bh)->b_state)
#define mark_buffer_uptodate(bh,i) set_bit(BH_Uptodate, &(bh)->b_state)
#define mark_buffer_clean(bh) clear_bit(BH_Dirty, &(bh)->b_state)



void __wait_on_buffer (struct buffer_head * bh);
struct buffer_head * getblk (int dev, int block, int size);
struct buffer_head * find_buffer (int dev, int block, int size);
struct buffer_head * get_hash_table(kdev_t dev, int block, int size);
struct buffer_head * bread (int dev, unsigned long block, size_t size);
int bwrite (struct buffer_head * bh);
void brelse (struct buffer_head * bh);
void bforget (struct buffer_head * bh);
void init_buffer_cache (void);
void refile_buffer (struct buffer_head * bh);
void file_buffer (struct buffer_head * bh, int list);
int fsync_dev (int dev);
void ll_rw_block (int rw, int nr, struct buffer_head * bh[]);
void check_and_free_buffer_mem (void);

#ifdef __alpha__

#define reiserfs_llseek lseek

#else

loff_t reiserfs_llseek (unsigned int fd, loff_t offset, unsigned int origin);

#endif /* __alpha__ */
