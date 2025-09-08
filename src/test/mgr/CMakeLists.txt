#unittest_mgr_activepymodule
#add_executable(unittest_mgr_activepymodule
#  test_activepymodule.cc)
#add_ceph_unittest(unittest_mgr_activepymodule)
#target_link_libraries(unittest_mgr_activepymodule global
#  ${CMAKE_SOURCE_DIR}/src/ceph_mgr.cc
#  ${CMAKE_SOURCE_DIR}/src/mon/PGMap.cc
#  ${CMAKE_SOURCE_DIR}/src/mon/ConfigMap.cc
#  ceph-common
#  Boost::python${MGR_PYTHON_VERSION_MAJOR}${MGR_PYTHON_VERSION_MINOR}
#  Python3::Python
#  cephsqlite
#  SQLite3::SQLite3
#  ${ALLOC_LIBS}
#  ${CMAKE_DL_LIBS}
#  ${GSSAPI_LIBRARIES})

# unittest_mgr_clusterstate
add_executable(unittest_mgr_clusterstate test_clusterstate.cc ${CMAKE_SOURCE_DIR}/src/mgr/ClusterState.cc)
add_ceph_unittest(unittest_mgr_clusterstate)
target_link_libraries(unittest_mgr_clusterstate
  ceph-common
  Python3::Python
  ${CMAKE_DL_LIBS}
  ${GSSAPI_LIBRARIES})

# unittest_mgr_daemonkey
add_executable(unittest_mgr_daemonkey test_daemonkey.cc ${CMAKE_SOURCE_DIR}/src/mgr/DaemonKey.cc)
add_ceph_unittest(unittest_mgr_daemonkey)
target_link_libraries(unittest_mgr_daemonkey ceph-common)

# unittest_mgr_mgrcap
add_executable(unittest_mgr_mgrcap
  test_mgrcap.cc
  $<TARGET_OBJECTS:mgr_cap_obj>)
add_ceph_unittest(unittest_mgr_mgrcap)
target_link_libraries(unittest_mgr_mgrcap global)

# unittest_mgr_pyformatter
add_executable(unittest_mgr_pyformatter test_pyformatter.cc ${CMAKE_SOURCE_DIR}/src/mgr/PyFormatter.cc)
add_ceph_unittest(unittest_mgr_pyformatter)
target_link_libraries(unittest_mgr_pyformatter ceph-common
  Python3::Python)

# unittest_mgr_pymodule
add_executable(unittest_mgr_pymodule test_pymodule.cc ${CMAKE_SOURCE_DIR}/src/mgr/PyModule.cc)
add_ceph_unittest(unittest_mgr_pymodule)
target_link_libraries(unittest_mgr_pymodule ceph-common
  Python3::Python ${CMAKE_DL_LIBS})


# unittest_mgr_pyutil
add_executable(unittest_mgr_pyutil test_pyutil.cc ${CMAKE_SOURCE_DIR}/src/mgr/PyUtil.cc)
add_ceph_unittest(unittest_mgr_pyutil)
target_link_libraries(unittest_mgr_pyutil ceph-common
  Python3::Python)

# unittest_mgr_pyservicemap
add_executable(unittest_mgr_servicemap test_servicemap.cc ${CMAKE_SOURCE_DIR}/src/mgr/ServiceMap.cc)
add_ceph_unittest(unittest_mgr_servicemap)
target_link_libraries(unittest_mgr_servicemap ceph-common
  Python3::Python)

# unittest_mgr_ttlcache
add_executable(unittest_mgr_ttlcache test_ttlcache.cc)
add_ceph_unittest(unittest_mgr_ttlcache)
target_link_libraries(unittest_mgr_ttlcache ceph-common
  Python3::Python ${CMAKE_DL_LIBS} ${GSSAPI_LIBRARIES})

#scripts
if(WITH_MGR_DASHBOARD_FRONTEND)
  if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|AARCH64|arm|ARM")
    add_ceph_test(mgr-dashboard-frontend-unittests ${CMAKE_SOURCE_DIR}/src/pybind/mgr/dashboard/run-frontend-unittests.sh)
  endif(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|AARCH64|arm|ARM")

  add_ceph_test(mgr-dashboard-smoke.sh ${CMAKE_CURRENT_SOURCE_DIR}/mgr-dashboard-smoke.sh)
endif(WITH_MGR_DASHBOARD_FRONTEND)
