#ifndef DKDISK_H_
#define DKDISK_H_

#include <sys/types.h>

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
};

#endif /* DKDISK_H_ */
