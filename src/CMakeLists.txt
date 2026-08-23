add_library(neuralkv_common
  common/status.cpp
)

target_include_directories(neuralkv_common PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(neuralkv_common PUBLIC cxx_std_20)

neuralkv_set_warnings(neuralkv_common)

add_library(neuralkv_storage
  storage/sharded_kv.cpp
)

target_include_directories(neuralkv_storage PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(neuralkv_storage PUBLIC neuralkv_common)

target_compile_features(neuralkv_storage PUBLIC cxx_std_20)

neuralkv_set_warnings(neuralkv_storage)

add_library(neuralkv_protocol
  protocol/codec.cpp
)

target_include_directories(neuralkv_protocol PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(neuralkv_protocol PUBLIC neuralkv_common)

target_compile_features(neuralkv_protocol PUBLIC cxx_std_20)

neuralkv_set_warnings(neuralkv_protocol)

add_library(neuralkv_net
  net/socket_utils.cpp
  net/connection.cpp
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(neuralkv_net PRIVATE net/epoll_server.cpp)
endif()

target_include_directories(neuralkv_net PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(neuralkv_net PUBLIC neuralkv_common neuralkv_protocol)

target_compile_features(neuralkv_net PUBLIC cxx_std_20)

neuralkv_set_warnings(neuralkv_net)

add_library(neuralkv_concurrency
  concurrency/thread_pool.cpp
)

target_include_directories(neuralkv_concurrency PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(neuralkv_concurrency PUBLIC neuralkv_common)

target_compile_features(neuralkv_concurrency PUBLIC cxx_std_20)

neuralkv_set_warnings(neuralkv_concurrency)

add_library(neuralkv_persistence
  persistence/crc32.cpp
  persistence/wal_record.cpp
  persistence/wal_writer.cpp
  persistence/recovery.cpp
  persistence/durable_storage.cpp
)

target_include_directories(neuralkv_persistence PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(neuralkv_persistence PUBLIC
  neuralkv_common
  neuralkv_storage
)

target_compile_features(neuralkv_persistence PUBLIC cxx_std_20)

neuralkv_set_warnings(neuralkv_persistence)

add_library(neuralkv_cluster
  cluster/cluster_config.cpp
  cluster/transport.cpp
)

target_include_directories(neuralkv_cluster PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(neuralkv_cluster PUBLIC
  neuralkv_common
  neuralkv_net
  neuralkv_protocol
)

target_compile_features(neuralkv_cluster PUBLIC cxx_std_20)

neuralkv_set_warnings(neuralkv_cluster)

add_library(neuralkv_raft
  raft/rpc_codec.cpp
  raft/log.cpp
  raft/node.cpp
)

target_include_directories(neuralkv_raft PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(neuralkv_raft PUBLIC
  neuralkv_common
  neuralkv_protocol
  neuralkv_persistence
  neuralkv_cluster
)

target_compile_features(neuralkv_raft PUBLIC cxx_std_20)

neuralkv_set_warnings(neuralkv_raft)

add_library(neuralkv_server
  server/handler.cpp
  server/connection_session.cpp
  server/blocking_server.cpp
  server/thread_pool_server.cpp
)

target_include_directories(neuralkv_server PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(neuralkv_server PUBLIC
  neuralkv_storage
  neuralkv_protocol
  neuralkv_net
  neuralkv_concurrency
  neuralkv_persistence
  neuralkv_cluster
  neuralkv_raft
)

target_compile_features(neuralkv_server PUBLIC cxx_std_20)

neuralkv_set_warnings(neuralkv_server)

# Connection and EpollServer (in neuralkv_net) dispatch through
# RequestHandler (in neuralkv_server), while the blocking/thread-pool
# servers (in neuralkv_server) open sockets through neuralkv_net. PRIVATE
# here still puts libneuralkv_server on the final link line wherever
# neuralkv_net is needed, without exposing it as part of neuralkv_net's
# own public interface to unrelated consumers (e.g. nkv-client).
target_link_libraries(neuralkv_net PRIVATE neuralkv_server)
