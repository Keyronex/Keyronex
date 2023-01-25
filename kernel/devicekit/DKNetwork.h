#ifndef DKNETWORK_H_
#define DKNETWORK_H_

#define MAC_FMT_PART "%02" PRIx8
#define MAC_FMT                                                         \
	MAC_FMT_PART ":" MAC_FMT_PART ":" MAC_FMT_PART ":" MAC_FMT_PART \
		     ":" MAC_FMT_PART ":" MAC_FMT_PART

#define MAC_ARG(MAC) mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]

#endif /* DKNETWORK_H_ */
