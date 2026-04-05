#ifndef ECX_LINUX_IF_VLAN_H
#define ECX_LINUX_IF_VLAN_H

enum vlan_ioctl_cmds {
	GET_VLAN_VID_CMD,
};

struct vlan_ioctl_args {
	int	cmd;
	char	device1[24];

	union {
		char	device2[24];
		int	VID;
		unsigned int skb_priority;
		unsigned int name_type;
		unsigned int flag;
	} u;

	short	vlan_qos;
};

#endif /* ECX_LINUX_IF_VLAN_H */
