#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "filesys/fat.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

/* On-disk inode.
 * Must be exactly DISK_SECTOR_SIZE bytes long. */
struct inode_disk
{

	// start : 파일의 inode인 경우 파일의 실제 내용,
	// 디렉토리의 inode인 경우 directory entry들이 저장되어 있는 disk의 sector 번호를 나타낸다.
	disk_sector_t start;  /* First data sector. */
	
	// 저장된 공간의 길이인데 섹터단위이다.
	off_t length;		  /* File size in bytes. */
	unsigned magic;		  /* Magic number. */
	uint32_t unused[124]; /* Not used. */

	// ----------project4 subdiretory start ----------
	// 디렉토리 구분 변수
	uint32_t is_dir; // true: dir, false: file
	// ----------project4 subdiretory end ----------
};

/* Returns the number of sectors to allocate for an inode SIZE
 * bytes long. */
static inline size_t
bytes_to_sectors(off_t size)
{
	return DIV_ROUND_UP(size, DISK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode
{
	struct list_elem elem;	/* Element in inode list. */
	disk_sector_t sector;	/* Sector number of disk location. */ // 디스크에서 몇번째 섹터인지(숫자임)
	int open_cnt;			/* Number of openers. */
	bool removed;			/* True if deleted, false otherwise. */
	int deny_write_cnt;		/* 0: writes ok, >0: deny writes. */
	//  디스크에 저장된 메타데이터 정보를 물리메모리에 올려놓은 것이다.
	// 매번 disk에 참조할 수 없기 때문에 물리 메모리에 올려놓고 사용하며,
	// 더이상 필요가 없어지면 inode_close()시에 다시 disk에 write back한다.
	struct inode_disk data; /* Inode content. */
};

/* Returns the disk sector that contains byte offset POS within
 * INODE.
 * Returns -1 if INODE does not contain data for a byte at offset
 * POS. */
static disk_sector_t
byte_to_sector(const struct inode *inode, off_t pos)
{
	ASSERT(inode != NULL);
	// 기존에는 그냥 다음 sector를 찾아가게 만들었음
	//////// 기존 코드 start
	// if (pos < inode->data.length)
	// 	return inode->data.start + pos / DISK_SECTOR_SIZE;
	// else
	// 	return -1;
	/////// 기존 코드 end

	//------project4-start-----------------------

	// fat을 보고 inode 찾아가게 만들기
	// if (pos < inode->data.length) {
	cluster_t start_clust = sector_to_cluster(inode->data.start); // start의 cluster index 받기
	while (pos >= DISK_SECTOR_SIZE)
	{ // pos가 속한 cluster 찾기
		///// file grow
		if (fat_get(start_clust) == EOChain)
		{								   // start_clust가 마지막 cluster이면
			fat_create_chain(start_clust); // 체인 하나 추가
		}
		//// file grow
		start_clust = fat_get(start_clust); // 다음 cluster 받기
		pos -= DISK_SECTOR_SIZE;
	}
	return cluster_to_sector(start_clust);
	// }
	// else
	// 	return -1;
	//------project4-end--------------------------
}

/* List of open inodes, so that opening a single inode twice
 * returns the same `struct inode'. */
// in-memory inode 전역변수 (Double linked list)
static struct list open_inodes;

/* Initializes the inode module. */
void inode_init(void)
{
	list_init(&open_inodes);
}

/* Initializes an inode with LENGTH bytes of data and
 * writes the new inode to sector SECTOR on the file system
 * disk.
 * Returns true if successful.
 * Returns false if memory or disk allocation fails. */
bool inode_create(disk_sector_t sector, off_t length, uint32_t is_dir)
{
	// inode_create : on-disk inode 생성하는 함수이다.
	// in-memory inode는 PM에만 존재하고 inode_open 때 만들어진다.
	// 3번째 인자로 uint32_t is_dir 추가 <- 파일,디렉터리 구분 인자 1이면 디렉토리, 0이면 파일
	struct inode_disk *disk_inode = NULL;
	bool success = false;

	ASSERT(length >= 0);

	/* If this assertion fails, the inode structure is not exactly
	 * one sector in size, and you should fix that. */
	ASSERT(sizeof *disk_inode == DISK_SECTOR_SIZE);

	disk_inode = calloc(1, sizeof *disk_inode); // disk_inode 1개 calloc으로 할당받기
	if (disk_inode != NULL)
	{
		size_t sectors = bytes_to_sectors(length); // 만들어야 할 sector의 개수
		disk_inode->length = length;			   // 인자로 받은 length 넣어주기
		disk_inode->magic = INODE_MAGIC;
		// inode 생성 시, struct inode_disk에 추가한 파일,디렉터리 구분
		// 을 위한 필드를 is_dir인자 값으로 설정
		disk_inode->is_dir = is_dir; // inode 생성 시, 파일,디렉터리 구분을 위한 필드를 is_dir인자 값으로 설정

		//------project4-start--------------------------------------------
		cluster_t new_cluster = fat_create_chain(0); // 새로운 체인 만들기
		if (new_cluster == 0)
		{ // 체인 만들기에 실패한 경우, 예외처리
			free(disk_inode);
			return success;
		}
		disk_inode->start = cluster_to_sector(new_cluster); // 새로운 체인을 만든 뒤에 해당 주소를 disk_inode->start값에 넣어주기
		disk_write(filesys_disk, sector, disk_inode);		// inode의 구조체(메타데이터) disk에 쓰기

		// cluster_t clst = sector_to_cluster(disk_inode->start);
		// cluster_t next_clst;
		// // sectors 개수만큼 클러스터 체인을 만들기
		// for (size_t i=1; i<sectors; i++) {
		// 	next_clst = fat_create_chain(clst);
		// 	if(next_clst == 0) {	// 체인 만들기에 실패한 경우 예외처리
		// 		free(disk_inode);
		// 		return success;
		// 	}
		// 	clst = next_clst;
		// }

		// inode(진짜 데이터들)를 저장하는 클러스터 체인을 모두 0으로 초기화
		if (sectors > 0)
		{
			//////////
			cluster_t clst = sector_to_cluster(disk_inode->start);
			cluster_t next_clst;
			// sectors 개수만큼 클러스터 체인을 만들기
			for (size_t i = 1; i < sectors; i++)
			{
				next_clst = fat_create_chain(clst);
				if (next_clst == 0)
				{ // 체인 만들기에 실패한 경우 예외처리
					free(disk_inode);
					return success;
				}
				clst = next_clst;
			}
			//////////
			static char zeros[DISK_SECTOR_SIZE];
			size_t i;
			disk_sector_t old_disk_sector = disk_inode->start;
			disk_sector_t new_disk_sector;
			for (i = 0; i < sectors; i++)
				disk_write(filesys_disk, old_disk_sector, zeros);
			new_disk_sector = cluster_to_sector(fat_get(sector_to_cluster(old_disk_sector)));
			old_disk_sector = new_disk_sector;
		}
		free(disk_inode); // mem에서 잠깐 사용한 temp buffer 느낌이므로 free해주기
		success = true;
		//------project4-end----------------------------------------------

		////////////////// 기존 코드 start
		// if (free_map_allocate (sectors, &disk_inode->start)) {
		// 	disk_write (filesys_disk, sector, disk_inode);		// inode의 메타데이터
		// 	if (sectors > 0) {
		// 		static char zeros[DISK_SECTOR_SIZE];
		// 		size_t i;

		// 		for (i = 0; i < sectors; i++)
		// 			disk_write (filesys_disk, disk_inode->start + i, zeros); // inode의 진짜 데이터를 0으로 초기화
		// 	}
		// 	success = true;
		// }
		// free (disk_inode);
		////////////////// 기존 코드 end
	}
	printf("[inode_create] success: %d\n", success);
	return success;
}

/* Reads an inode from SECTOR
 * and returns a `struct inode' that contains it.
 * Returns a null pointer if memory allocation fails. */
struct inode *
inode_open(disk_sector_t sector)
{
	struct list_elem *e;
	struct inode *inode;

	/* Check whether this inode is already open. */
	for (e = list_begin(&open_inodes); e != list_end(&open_inodes);
		 e = list_next(e))
	{
		inode = list_entry(e, struct inode, elem);
		if (inode->sector == sector)
		{
			inode_reopen(inode);
			return inode;
		}
	}

	/* Allocate memory. */
	inode = malloc(sizeof *inode);
	if (inode == NULL)
		return NULL;

	/* Initialize. */
	list_push_front(&open_inodes, &inode->elem);
	inode->sector = sector;
	inode->open_cnt = 1;
	inode->deny_write_cnt = 0;
	inode->removed = false; // 삭제되면 true로 바꿈
	disk_read(filesys_disk, inode->sector, &inode->data);
	return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen(struct inode *inode)
{
	if (inode != NULL)
		inode->open_cnt++;
	return inode;
}

/* Returns INODE's inode number. */
disk_sector_t
inode_get_inumber(const struct inode *inode)
{
	return inode->sector;
}

/* Closes INODE and writes it to disk.
 * If this was the last reference to INODE, frees its memory.
 * If INODE was also a removed inode, frees its blocks. */
void inode_close(struct inode *inode)
{
	/* Ignore null pointer. */
	if (inode == NULL)
		return;

	/* Release resources if this was the last opener. */
	if (--inode->open_cnt == 0)
	{ // open_cnt가 0일 경우에만 inode를 삭제해준다.
		/* Remove from inode list and release lock. */
		list_remove(&inode->elem);

		//------project4-start------------------------
		if (inode->removed)
		{
			fat_remove_chain(sector_to_cluster(inode->sector), 0);	   // inode 구조체(메타데이터) fat에서 제거
			fat_remove_chain(sector_to_cluster(inode->data.start), 0); // inode 실제 데이터들 모두를 fat에서 제거
		}
		// 기존 파일 크기보다 더 크게 write를 한 경우, disk에 업데이트 해 주어야 함
		disk_write(filesys_disk, inode->sector, &inode->data);
		free(inode);
		//------project4-end--------------------------

		//////// 기존 코드 start
		// /* Deallocate blocks if removed. */
		// if (inode->removed) {
		// 	free_map_release (inode->sector, 1);
		// 	free_map_release (inode->data.start,
		// 			bytes_to_sectors (inode->data.length));
		// }
		//////// 기존 코드 end
	}
}

/* Marks INODE to be deleted when it is closed by the last caller who
 * has it open. */
void inode_remove(struct inode *inode)
{
	ASSERT(inode != NULL);
	inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
 * Returns the number of bytes actually read, which may be less
 * than SIZE if an error occurs or end of file is reached. */
off_t inode_read_at(struct inode *inode, void *buffer_, off_t size, off_t offset)
{
	uint8_t *buffer = buffer_;
	off_t bytes_read = 0;
	uint8_t *bounce = NULL;
	while (size > 0)
	{
		/* Disk sector to read, starting byte offset within sector. */
		disk_sector_t sector_idx = byte_to_sector(inode, offset);
		int sector_ofs = offset % DISK_SECTOR_SIZE;

		/* Bytes left in inode, bytes left in sector, lesser of the two. */
		off_t inode_left = inode_length(inode) - offset;
		int sector_left = DISK_SECTOR_SIZE - sector_ofs;
		int min_left = inode_left < sector_left ? inode_left : sector_left;

		/* Number of bytes to actually copy out of this sector. */
		int chunk_size = size < min_left ? size : min_left;
		if (chunk_size <= 0)
			break;

		if (sector_ofs == 0 && chunk_size == DISK_SECTOR_SIZE)
		{
			/* Read full sector directly into caller's buffer. */
			disk_read(filesys_disk, sector_idx, buffer + bytes_read);
		}
		else
		{
			/* Read sector into bounce buffer, then partially copy
			 * into caller's buffer. */
			if (bounce == NULL)
			{
				bounce = malloc(DISK_SECTOR_SIZE);
				if (bounce == NULL)
					break;
			}
			disk_read(filesys_disk, sector_idx, bounce);
			memcpy(buffer + bytes_read, bounce + sector_ofs, chunk_size);
		}

		/* Advance. */
		size -= chunk_size;
		offset += chunk_size;
		bytes_read += chunk_size;
	}
	free(bounce);
	return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
 * Returns the number of bytes actually written, which may be
 * less than SIZE if end of file is reached or an error occurs.
 * (Normally a write at end of file would extend the inode, but
 * growth is not yet implemented.) */
off_t inode_write_at(struct inode *inode, const void *buffer_, off_t size,
					 off_t offset)
{
	const uint8_t *buffer = buffer_;
	off_t bytes_written = 0;
	uint8_t *bounce = NULL;

	if (inode->deny_write_cnt)
		return 0;

	// 만약 write를 통해서 file의 크기가 늘어나야 한다면, offset+size만큼 file의 크기 늘려주기
	if (offset + size > inode->data.length)
	{
		inode->data.length = offset + size;
	}

	while (size > 0)
	{
		/* Sector to write, starting byte offset within sector. */
		disk_sector_t sector_idx = byte_to_sector(inode, offset);
		int sector_ofs = offset % DISK_SECTOR_SIZE;

		/* Bytes left in inode, bytes left in sector, lesser of the two. */
		off_t inode_left = inode_length(inode) - offset;
		int sector_left = DISK_SECTOR_SIZE - sector_ofs;
		int min_left = inode_left < sector_left ? inode_left : sector_left; // file의 크기보다 더 크게 쓸 수 있도록 해야 함

		/* Number of bytes to actually write into this sector. */
		int chunk_size = size < min_left ? size : min_left;
		if (chunk_size <= 0)
			break;

		if (sector_ofs == 0 && chunk_size == DISK_SECTOR_SIZE)
		{
			/* Write full sector directly to disk. */
			disk_write(filesys_disk, sector_idx, buffer + bytes_written);
		}
		else
		{
			/* We need a bounce buffer. */
			if (bounce == NULL)
			{
				bounce = malloc(DISK_SECTOR_SIZE);
				if (bounce == NULL)
					break;
			}

			/* If the sector contains data before or after the chunk
			   we're writing, then we need to read in the sector
			   first.  Otherwise we start with a sector of all zeros. */
			if (sector_ofs > 0 || chunk_size < sector_left)
				disk_read(filesys_disk, sector_idx, bounce);
			else
				memset(bounce, 0, DISK_SECTOR_SIZE);
			memcpy(bounce + sector_ofs, buffer + bytes_written, chunk_size);
			disk_write(filesys_disk, sector_idx, bounce);
		}

		/* Advance. */
		size -= chunk_size;
		offset += chunk_size;
		bytes_written += chunk_size;
	}
	free(bounce);

	return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void inode_deny_write(struct inode *inode)
{
	inode->deny_write_cnt++;
	ASSERT(inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
 * Must be called once by each inode opener who has called
 * inode_deny_write() on the inode, before closing the inode. */
void inode_allow_write(struct inode *inode)
{
	ASSERT(inode->deny_write_cnt > 0);
	ASSERT(inode->deny_write_cnt <= inode->open_cnt);

	inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t inode_length(const struct inode *inode)
{
	return inode->data.length;
}

//------project4-start---------------------------------------------------
// fd 리스트에서 fd에 대한 file 정보를 얻어옴
// fd의 in-memory inode가 디렉터리 인지 판단하여 성공여부 반환
bool inode_is_dir(const struct inode *inode)
{
	bool result;
	/* inode_disk 자료구조를 메모리에 할당 */
	struct inode_disk *disk_inode = calloc(1, sizeof *disk_inode);

	/* in-memory inode의 on-disk inode를 읽어 inode_disk에 저장 */
	disk_read(filesys_disk, cluster_to_sector(inode->sector), disk_inode);

	/* on-disk inode의 is_dir을 result에 저장하여 반환 */
	result = disk_inode->is_dir; // 1 or 0이 있을거임.

	return result;
}

//------project4-end-----------------------------------------------------
