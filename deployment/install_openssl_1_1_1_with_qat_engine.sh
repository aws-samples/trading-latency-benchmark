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
git clone https://github.com/intel/ipp-crypto.git
cd ipp-crypto
git checkout ippcp_2021.7.1
cd ./sources/ippcp/crypto_mb
export OPENSSL_ROOT_DIR=/opt/openssl/1.1.1e/
cmake . -Bbuild -DCMAKE_INSTALL_PREFIX=/opt/crypto_mb/2021.7.1
cd build
make -j
make install
git clone https://github.com/intel/intel-ipsec-mb.git
cd intel-ipsec-mb
git checkout v1.3
make -j
make install NOLDCONFIG=y PREFIX=/opt/ipsec-mb/1.3
git clone https://github.com/intel/QAT_Engine.git
cd QAT_Engine
git checkout v1.2.0
./autogen.sh
export LDFLAGS="-L/opt/ipsec-mb/1.3/lib -L/opt/crypto_mb/2021.7.1/lib"
export CPPFLAGS="-I/opt/ipsec-mb/1.3/include -I/opt/crypto_mb/2021.7.1/include"
./configure --prefix=/opt/openssl/1.1.1e --with-openssl_install_dir=/opt/openssl/1.1.1e \
--with-openssl_dir=/home/ec2-user/openssl-1.1.1e \
--enable-qat_sw \
#--enable-qat_debug \
#--with-qat_debug_file=/home/ec2-user/qat.log
PERL5LIB=/home/ec2-user/openssl-1.1.1e make -j
PERL5LIB=/home/ec2-user/openssl-1.1.1e make install
ls -l /opt/openssl/1.1.1e/lib/engines-1.1/qatengine.*
export LD_LIBRARY_PATH=/opt/openssl/1.1.1e/lib:/opt/crypto_mb/2021.7.1/lib:/opt/ipsec-mb/1.3/lib
echo "LD_LIBRARY_PATH=/opt/openssl/1.1.1e/lib:/opt/crypto_mb/2021.7.1/lib:/opt/ipsec-mb/1.3/lib" >> ~/.bashrc
echo "LD_LIBRARY_PATH=/opt/openssl/1.1.1e/lib:/opt/crypto_mb/2021.7.1/lib:/opt/ipsec-mb/1.3/lib" >> /home/ec2-user/.bashrc
export OPENSSL_LIBS=ssl:crypto_mb:IPSec_MB:crypto
echo "export OPENSSL_LIBS=ssl:crypto_mb:IPSec_MB:crypto" >> ~/.bashrc
echo "export OPENSSL_LIBS=ssl:crypto_mb:IPSec_MB:crypto" >> /home/ec2-user/.bashrc
cp -r /opt/crypto_mb/2021.7.1/lib/*  /opt/openssl/1.1.1e/lib/
cp -r /opt/crypto_mb/2021.7.1/lib/*  /usr/lib64/
cp -r /opt/ipsec-mb/1.3/lib/* /opt/openssl/1.1.1e/lib/
cp -r /opt/ipsec-mb/1.3/lib/* /usr/lib64/
ln -sf /opt/openssl/1.1.1e/lib/libcrypto.so.1.1 /usr/lib64/libcrypto.so.1.1
ln -sf /opt/openssl/1.1.1e/lib/libssl.so.1.1 /usr/lib64/libssl.so.1.1
