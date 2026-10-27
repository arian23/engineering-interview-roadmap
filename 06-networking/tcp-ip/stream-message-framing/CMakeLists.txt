add_program(stream_message_framing)
# Reuse the RAII socket helpers from the TCP server exercise.
target_include_directories(stream_message_framing PRIVATE ${PROJECT_SOURCE_DIR}/06-networking/sockets/concurrent-tcp-server)
