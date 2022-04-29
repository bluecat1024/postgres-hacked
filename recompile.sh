#!/bin/bash
pg_ctl -D /usr/local/pgsql/data -l logfile stop
sudo rm -rf /usr/local/pgsql/data/*
cd 15712-Project
sudo make uninstall
./configure --with-llvm
make
sudo make install
cd ..
initdb -D /usr/local/pgsql/data
pg_ctl -D /usr/local/pgsql/data -l logfile start
createdb test
truncate logfile -s 0
