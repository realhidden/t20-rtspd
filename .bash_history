mkdir /build
cd /build
apt-get update
apt-get install -y p7zip wget git build-essential
wget https://github.com/Dafang-Hacks/Ingenic-T10_20/raw/master/resource/toolchain/mips-gcc472-glibc216-64bit-r2.3.3.7z
p7zip -d mips-gcc472-glibc216-64bit-r2.3.3.7z
export PATH=/build/mips-gcc472-glibc216-64bit/bin/:$PATH
cd ~
make
make
make
make
make clean
make
mkdir /buildal
ls -al
ls -alh
make clean
make
make
make
make
make
make clean
make
make
make
make
ls -al
ls -alh
