#ifndef DKDISK_H_
#define DKDISK_H_

#include <sys/types.h>

#include <devicekit/DKDevice.h>
#include <vm/vm.h>

@class DKDrive;
@protocol DKDriveMethods;

/*!
 * Kind of block I/O.
 */
typedef enum dk_strategy {
	kDKRead,
	kDKWrite,
} dk_strategy_t;

/*!
 * Represents an I/O operation. The initiator of the operation allocates one of
 * these and passes it to a method; the initiator is responsible for freeing the
 * structure, but must ensure not to do so before the operation is completed.
 */
struct dk_diskio_completion {
	/*!
	 * Function to be called when the I/O completes.
	 * @param data the completion's data member
	 * @param result number of writes read/writen, or -errno for error
	 */
	void (*callback)(void *data, ssize_t result);
	/*! Opaque data passed to callback. */
	void *data;

	/*! These fields are for use by DKDrive only. */
	/*! Drive we are acting on. */
	DKDrive<DKDriveMethods> *drive;
	/*! Strategy */
	dk_strategy_t strategy;
	/*! Initial block. */
	blkoff_t initial;
	/*! Buffer */
	vm_mdl_t *buf;
	/*! Storage for final callback during split-up I/O. */
	void (*final_callback)(void *data, ssize_t result);
	/*! Storage for final data during split-up I/O. */
	void *final_data;
	/*! Bytes remaining to read/write. */
	size_t bytes_left;
	/*! Bytes total to do. */
	size_t bytes_total;
};

/*!
 * Protocol common to physical and logical drives.
 */
@protocol DKAbstractDiskMethods

/*! block size in byets */
@property (readonly) blksize_t blockSize;

- (int)readBytes:(size_t)nBytes
	      at:(off_t)offset
      intoBuffer:(vm_mdl_t *)buf
      completion:(struct dk_diskio_completion *)completion;
- (int)writeBytes:(size_t)nBytes
	       at:(off_t)offset
       fromBuffer:(vm_mdl_t *)buf
       completion:(struct dk_diskio_completion *)completion;

@end

/*!
 * Physical disk methods. @see DKDisk
 */
@protocol DKDriveMethods

- (int)strategy:(dk_strategy_t)strategy
	 blocks:(blksize_t)nBlocks
	     at:(blkoff_t)offset
	 buffer:(vm_mdl_t *)buf
     completion:(struct dk_diskio_completion *)completion;

@end

/*!
 * The abstract superclass of physical disks proper which are present in a
 * drive. The class provides methods which handle the generic aspects of block
 * I/O, such as deblocking.
 *
 * Implementors must implement the DKDriveMethods protocol; its methods carry
 * out the actual I/O.
 */
@interface DKDrive : DKDevice <DKAbstractDiskMethods> {
	int	  m_driveID;
	blksize_t m_blockSize;
	blkcnt_t  m_nBlocks;
	blkcnt_t  m_maxBlockTransfer;
}

/*! Unique drive identifier. TODO: move into DKDrive */
@property (readonly) int driveID;

/*! total size in unit blockSize */
@property (readonly) blkcnt_t nBlocks;

/*! Maximum number of blocks transferrable in a single operation. */
@property (readonly) blkcnt_t maxBlockTransfer;

@end

/*!
 * Provides an abstract interface to a logical disk; a logical disk may refer to
 * a physical disk, or to a partition, or any other sort of random-access media.
 * It also provides a node in the POSIX DevFS and adapts operations to
 * the DeviceKit interface.
 *
 * By default it passes all operations up to an underlying disk object, with an
 * offset. The class is thus suitable for immediate use to adapt a
 * DKDrive, both for the whole disk and for simple volume management
 * schemes, e.g. GPT or FDisk. More complex schemes will need to subclass.
 */
@interface DKLogicalDisk : DKDevice <DKAbstractDiskMethods> {
	DKDevice<DKAbstractDiskMethods> *m_underlying;
	off_t				 m_base;
	size_t				 m_size;
	size_t				 m_location;

	/*! entry in ld_tailq */
	TAILQ_TYPE_ENTRY(DKLogicalDisk) m_ld_tailq_entry;
	/*! minor number */
	int m_minor;
}

/*!
 * Underlying disk device.
 */
@property (readonly) DKDevice<DKAbstractDiskMethods> *underlying;

/*!
 * Offset (in bytes) from the underlying disk device.
 */
@property (readonly) off_t base;

/*!
 * Size (in bytes) of the logical disk.
 */
@property (readonly) size_t size;

/*!
 * Logical location relative to parent. If 0, this is regarded as being the root
 * of a particular tree of devices (i.e. it represents the physical disk
 * itself).
 * @todo adapt to be flexible & move this into DKDevice
 */
@property (readonly) size_t location;

- initWithUnderlyingDisk:(DKDevice<DKAbstractDiskMethods> *)underlying
		    base:(off_t)base
		    size:(size_t)size
		    name:(const char *)name
		location:(size_t)location
		provider:(DKDevice *)provider;

@end

#endif /* DKDISK_H_ */
