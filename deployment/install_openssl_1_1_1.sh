yum install -y wget autoconf automake libtool cmake pkg-config perl gcc make perl
yum group install -y "Development Tools"
curl -O https://www.nasm.us/pub/nasm/releasebuilds/2.16.01/nasm-2.16.01.tar.gz
tar xzf nasm-2.16.01.tar.gz
cd nasm-2.16.01
./configure
make
make install
export PATH=/usr/local/bin/:$PATH
nasm -v
cd ..
wget https://github.com/openssl/openssl/archive/refs/tags/OpenSSL_1_1_1e.tar.gz
tar -xf OpenSSL_1_1_1e.tar.gz
mv openssl-OpenSSL_1_1_1e openssl-1.1.1e
cd openssl-1.1.1e
./config --prefix=/opt/openssl/1.1.1e --openssldir=/opt/openssl/1.1.1e
make -j
make install
export PATH=/opt/openssl/1.1.1e/bin:$PATH
echo "export PATH=/opt/openssl/1.1.1e/bin:$PATH" >> ~/.bashrc
export LD_LIBRARY_PATH=/opt/openssl/1.1.1e/lib
echo "LD_LIBRARY_PATH=/opt/openssl/1.1.1e/lib" >> ~/.bashrc
echo "LD_LIBRARY_PATH=/opt/openssl/1.1.1e/lib" >> /home/ec2-user/.bashrc
echo "export OPENSSL_LIBS=ssl:crypto" >> ~/.bashrc
echo "export OPENSSL_LIBS=ssl:crypto" >> /home/ec2-user/.bashrc
ln -sf /opt/openssl/1.1.1e/lib/libcrypto.so.1.1 /usr/lib64/libcrypto.so.1.1
ln -sf /opt/openssl/1.1.1e/lib/libssl.so.1.1 /usr/lib64/libssl.so.1.1
