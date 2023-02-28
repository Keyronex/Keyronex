while true; do
/usr/libexec/virtiofsd  --socket-path=/tmp/vhostqemu -o source=`pwd` -o cache=always -d --socket-group=wheel
echo "Restarting this stupid fucker of a thing."
done
