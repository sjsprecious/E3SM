if (NOT DEBUG)
  string(APPEND CFLAGS " -O2")
  string(APPEND FFLAGS " -O2")
endif()
string(APPEND CPPDEFS " -DLINUX")
if (DEBUG)
  string(APPEND FFLAGS " -C -traceback -Mchkfpstk -Mchkstk -Mdalign  -Mdepchk -Miomutex -Mrecursive  -Meh_frame -Ktrap=fp")
endif()
if (COMP_NAME STREQUAL eam)
  string(APPEND FFLAGS " -Mnovect")
endif()
if (COMP_NAME STREQUAL cice)
  string(APPEND FFLAGS " -Mnovect")
endif()
set(NETCDF_PATH "$ENV{NETCDF_HOME}")
set(PIO_FILESYSTEM_HINTS "lustre")
set(PNETCDF_PATH "$ENV{PNETCDF_HOME}")
string(APPEND SLIBS " -lpmi -L${NETCDF_PATH}/lib -lnetcdf -lnetcdff -L$ENV{MKL_PATH}/lib/intel64/ -lmkl_rt $ENV{PNETCDF_LIBRARIES}")
if (MPILIB STREQUAL impi)
  set(MPICC "mpipgcc")
  set(MPICXX "mpipgcxx")
  set(MPIFC "mpipgf90")
endif()