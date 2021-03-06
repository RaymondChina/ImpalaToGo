#!/bin/bash 
#parameters are
# $1 access key
# $2 secret key
# $3 DNS of the master 
# $4 S3 backet with data
echo "Attaching to cluster"

ACCESS_KEY=$1
SECRET_KEY=$2
MASTER_DNS=$3
S3_BACKET=$4

#get script path
SCRIPT=$(readlink -f "$0")
SCRIPTPATH=$(dirname "$SCRIPT")

eval sed -e 's/ACCESS_KEY/${ACCESS_KEY}/g' -e 's/SECRET_KEY/${SECRET_KEY}/g' -e 's/S3_BACKET/${S3_BACKET}/g'  $SCRIPTPATH/conf/hdfs-site.template > hdfs-site.xml
eval sed -e 's/ACCESS_KEY/${ACCESS_KEY}/g' -e 's/SECRET_KEY/${SECRET_KEY}/g' $SCRIPTPATH/conf/hive-site.template > hive-site.xml
eval sed 's/MASTER_DNS/${MASTER_DNS}/g' $SCRIPTPATH/conf/impala_defaults.template >impala


sudo cp hdfs-site.xml /etc/impala/conf
sudo cp hive-site.xml /etc/hive/conf/
sudo cp hive-site.xml /etc/alternatives/hive-conf/
sudo cp impala /etc/default/

rm hdfs-site.xml
rm hive-site.xml
rm impala


$SCRIPTPATH/linkEphimerialDrive.sh
