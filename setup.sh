apt-get install build-essential
apt-get install pkg-config autoconf automake autogen libpcap-dev libtool
./autogen.sh
./configure
make
cp -r ../nDPI/ /usr/local/nDPI
