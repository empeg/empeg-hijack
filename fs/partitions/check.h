/*
 * add_partition adds a partitions details to the devices partition
 * description.
 */
void add_gd_partition(struct gendisk *hd, int minor, int start, int size, int type);

/*
 * Get the default block size for this device
 */
unsigned int get_ptable_blocksize(kdev_t dev);
