cd 15712-Project
./configure --with-llvm
make
sudo make install
sudo mkdir /usr/local/pgsql/data
sudo chown postgres /usr/local/pgsql/data
initdb -D /usr/local/pgsql/data
pg_ctl -D /usr/local/pgsql/data -l logfile start
createdb test
