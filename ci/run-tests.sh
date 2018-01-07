#!/bin/sh
#
# Test Git
#

. ${0%/*}/lib-travisci.sh

mkdir -p $HOME/travis-cache
ln -s $HOME/travis-cache/.prove t/.prove
make --quiet test
for config in $test_configurations
do
	eval $config make --quiet test
done
