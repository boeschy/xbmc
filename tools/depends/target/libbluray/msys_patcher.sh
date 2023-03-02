SCRIPT_DIR=`dirname $0`
BLD_SRC_DIR=$1


patch -p1 -d "${BLD_SRC_DIR}" -i "${SCRIPT_DIR}/0001-Return-functions-required-for-3D-MVC-identification.patch" || echo "Failed to patch - skipping!"
