#ifndef DKGPTVOLUMEMANAGER_H_
#define DKGPTVOLUMEMANAGER_H_

#include <devicekit/DKDisk.h>

@interface GPTVolumeManager : DKDevice

+ (BOOL)probe:(DKLogicalDisk *)disk;

@end


#endif /* DKGPTVOLUMEMANAGER_H_ */
