## PostgreSQL Database Management System
This directory contains the source code distribution of the PostgreSQL
database management system.

PostgreSQL is an advanced object-relational database management system
that supports an extended subset of the SQL standard, including
transactions, foreign keys, subqueries, triggers, user-defined types
and functions.  This distribution also contains C language bindings.

PostgreSQL has many language interfaces, many of which are listed here:

	https://www.postgresql.org/download/

See the file INSTALL for instructions on how to build and install
PostgreSQL.  That file also lists supported operating systems and
hardware platforms and contains information regarding any other
software packages that are required to build or run the PostgreSQL
system.  Copyright and license information can be found in the
file COPYRIGHT.  A comprehensive documentation set is included in this
distribution; it can be read as described in the installation
instructions.

The latest version of this software may be obtained at
https://www.postgresql.org/download/.  For more information look at our
web site located at https://www.postgresql.org/.

## Added by Jiuzhi:
Environment setup: (for details, refer to: https://www.postgresql.org/docs/current/installation.html)
1. Create a EC2 machine with Ubuntu 20.04. Sign in to the VM using the user name "ubuntu".
2. Install packages:
```bash
sudo apt update
sudo apt upgrade
sudo install gcc make libreadline8 libreadline-dev zlib1g zlib1g-dev  build-essential bison postgresql-client
```
3. Clone the repository and use the master branch (it is the REL_14_STABLE for the postgres original repo).
4. Build and install postgres
```bash
./configure
make
sudo make install
```
5. Set the environment variable for ubuntu. You can add it to ~/.bashrc
```bash
LD_LIBRARY_PATH=/usr/local/pgsql/lib
export LD_LIBRARY_PATH
PATH=/usr/local/pgsql/bin:$PATH
export PATH
```
6. Start the postgres:
```bash
# Create the data folder
sudo mkdir /usr/local/pgsql/data
sudo chown postgres /usr/local/pgsql/data
initdb -D /usr/local/pgsql/data
pg_ctl -D /usr/local/pgsql/data -l logfile start
```
7. Play with the psql
