#!/bin/bash

function all()
{
	prefix=$TARGET_TMPROOTFS_DIR

	# real build
	make -j${N} all || {
	echo "make failed"
	exit 1
	}

	# put include, lib& binaries to dest directory
	install -D -m 0644 inc/uvc.h $prefix/include/guvc/uvc.h
	install -D -m 0644 inc/uvc_gadget.h $prefix/include/guvc/uvc_gadget.h
	install -D -m 0644 inc/uvc_gadget_api.h $prefix/include/guvc/uvc_gadget_api.h
	install -p -D -m 755 ${BUILD_OUTPUT_PATH}/uvc-gadget $prefix/usr/bin/uvc-gadget
	install -p -D -m 755 ${BUILD_OUTPUT_PATH}/libguvc.so $prefix/lib/libguvc.so
	install -p -D -m 755 ${BUILD_OUTPUT_PATH}/libguvc.a $prefix/lib/libguvc.a
}

function all_32()
{
	all
}

function clean()
{
	make dist-clean
}

# include
. $INCLUDE_FUNCS
# include end

dependon libmm

cd $(dirname $0)

buildopt $1
