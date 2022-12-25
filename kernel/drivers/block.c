#include <drivers/block.h>
#include <fs/cdev.h>
#include <debug.h>

static int register_mbr_partitions(struct blkdev *blkdev);
static int register_gpt_partitions(struct blkdev *blkdev);
static int detect_filesystems(struct blkdev *blkdev);
static ssize_t partition_device_read(struct file_handle *, void *, size_t, off_t);
static ssize_t partition_device_write(struct file_handle *, const void *, size_t, off_t);

static struct file_ops partition_fops = {
	.read = partition_device_read,
	.write = partition_device_write,
	.ioctl = NULL,
	.shared = NULL
};

static ssize_t partition_device_read(struct file_handle *handle, void *buffer, size_t cnt,  off_t offset) {
	struct partition *partition = handle->private_data;
	struct blkdev *blkdev = partition->blkdev;

	if((offset + cnt) > (partition->lba_cnt * blkdev->disk->stat->st_blksize)) {
		return -1;
	}

	return blkdev->disk->ops->read(blkdev->disk, buffer, cnt, offset + partition->lba_start * blkdev->disk->stat->st_blksize);
}

static ssize_t partition_device_write(struct file_handle *handle, const void *buffer, size_t cnt,  off_t offset) {
	struct partition *partition = handle->private_data;
	struct blkdev *blkdev = partition->blkdev;

	if((offset + cnt) > (partition->lba_cnt * blkdev->disk->stat->st_blksize)) {
		return -1;
	}

	return blkdev->disk->ops->write(blkdev->disk, buffer, cnt, offset + partition->lba_start * blkdev->disk->stat->st_blksize);
}

int register_blkdev(struct blkdev *blkdev) {
	print("block: %s storage device:\n", (blkdev->device_name == NULL) ? "unkown" : blkdev->device_name);

	if(blkdev->serial_number) print("block: serial number: %s\n", blkdev->serial_number); 
	if(blkdev->firmware_revision) print("block: firmwarm revision: %s\n", blkdev->firmware_revision);
	if(blkdev->model_number) print("block: model number: %s\n", blkdev->model_number);

	if(register_mbr_partitions(blkdev) == -1) {
		if(register_gpt_partitions(blkdev) == -1) {
			print("block: no partitions detected\n");
			return 0;
		}
	}

	struct partition *partition = blkdev->partitions;

	while(partition) {
		struct cdev *partition_cdev = alloc(sizeof(struct cdev));

		partition_cdev->fops = &partition_fops;
		partition_cdev->private_data = partition;
		partition_cdev->rdev = makedev(blkdev->partition_major, blkdev->partition_minor);

		cdev_register(partition_cdev);

		struct stat *stat = alloc(sizeof(struct stat));
		stat_init(stat);

		stat->st_blksize = blkdev->disk->stat->st_blksize;
		stat->st_mode = S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
		stat->st_rdev = makedev(blkdev->partition_major, blkdev->partition_minor);

		char *partition_path = alloc(MAX_PATH_LENGTH);
		sprint(partition_path, "%s%d", blkdev->device_prefix, blkdev->partition_minor);

		struct file_handle *handle = alloc(sizeof(struct file_handle));

		handle->private_data = partition;
		handle->ops = &partition_fops;
		handle->stat = stat;

		partition->partition_path = partition_path;
		partition->handle = handle;
		partition->blkdev = blkdev;

		vfs_create_node_deep(NULL, NULL, NULL, stat, partition_path);

		print("block: partition: [%s] [%x:%x] [%x -> %x]\n", partition_path, blkdev->partition_major, blkdev->partition_minor, partition->lba_start, partition->lba_start + partition->lba_cnt);

		blkdev->partition_minor++;
		partition = partition->next;
	}

	detect_filesystems(blkdev);

	return 0;
}

static int detect_filesystems(struct blkdev *blkdev) {
	struct partition *partition = blkdev->partitions;

	while(partition) {
		if(ext2_init(partition) != -1) {
			partition = partition->next;
			continue;
		}

		partition = partition->next;
	}

	return 0;
}

static int register_mbr_partitions(struct blkdev *blkdev) {
	void *lba = alloc(blkdev->disk->stat->st_blksize);

	if(blkdev->disk->ops->read(blkdev->disk, lba, blkdev->disk->stat->st_blksize, 0) == -1) {
		print("block: read error from disk\n");
		return -1;
	}

	uint16_t mbr_signature = *(uint16_t*)(lba + 510);

	if(mbr_signature != MBR_SIGNATURE) {
		return -1;
	}

	struct mbr_partition *mbr_partition = lba + 0x1be;

	for(int i = 0; i < 4; i++, mbr_partition++) {
		if(mbr_partition->type == 0 || mbr_partition->type == 0xee) {
			continue;
		}

		struct partition *partition = alloc(sizeof(struct partition));

		partition->lba_start = mbr_partition->lba_start;
		partition->lba_cnt = mbr_partition->lba_cnt;

		partition->next = blkdev->partitions;
		blkdev->partitions = partition;
	}

	return 0;
}

static int register_gpt_partitions(struct blkdev *blkdev) {
	void *lba = alloc(blkdev->disk->stat->st_blksize);

	if(blkdev->disk->ops->read(blkdev->disk, lba, blkdev->disk->stat->st_blksize, blkdev->disk->stat->st_blksize) == -1) {
		print("partition: read error from disk\n");
		return -1;
	}

	struct gpt_partition_table *gpt_hdr = lba;

	if(gpt_hdr->identifier != GPT_SIGNATURE) {
		return -1;
	}

	return 0;
}
