AM_CXXFLAGS = -I$(srcdir)/src -I$(srcdir)/include/libpbkdf2-compute-opencl

lib_LTLIBRARIES = libpbkdf2-compute-opencl.la

AM_CXXFLAGS += -I$(srcdir)/../libpbkdf2-gpu-common/include
AM_CXXFLAGS += -I$(srcdir)/../libhashspec-hashalgorithm/include

libpbkdf2_compute_opencl_la_LIBADD = ../libhashspec-hashalgorithm/libhashspec-hashalgorithm.la -l$(OPENCL_LIB)
libpbkdf2_compute_opencl_la_SOURCES = \
    src/device.cpp \
    src/globalcontext.cpp \
    src/kernelloader.cpp \
    src/hashfunctioncontext.cpp \
    src/computecontext.cpp \
    src/devicecontext.cpp \
    src/processingunit.cpp \
    src/kernelloader.h \
    src/alignment.h

dist_data_DATA = data/opencl/kernels/pbkdf2_sha1.cl

SUBDIRS = . include
