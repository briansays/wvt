# Two wvt programs on the loopback (issue #96): one publishes the simulator
# with --publish, the other receives it with --device zmq and records 1 s.
# The recording must have all the samples of that second, with no gap.
# -DWVT=<wvt> -DDIR=<scratch folder>
file(REMOVE_RECURSE ${DIR})
file(MAKE_DIRECTORY ${DIR})
string(RANDOM LENGTH 2 ALPHABET 0123456789 r)
set(addr 127.0.0.1:159${r})
# execute_process runs its commands at the same time (as a pipeline). The
# publisher records 4 s, so it stops by itself; the subscriber waits up to 3 s
# for the stream.
execute_process(
  COMMAND ${WVT} --no-log-file --no-config --device sim -c 100M -r 1M --publish ${addr} --no-gui
          --record ${DIR}/pub --record-duration 4
  COMMAND ${WVT} --no-log-file --no-config --device zmq:${addr} --no-gui --record ${DIR}/sub --record-duration 1
  WORKING_DIRECTORY ${DIR}
  RESULTS_VARIABLE rcs OUTPUT_VARIABLE out ERROR_VARIABLE out)
message("${out}")
if(NOT rcs STREQUAL "0;0")
  message(FATAL_ERROR "exit codes (publisher;subscriber): ${rcs}")
endif()
file(GLOB data ${DIR}/sub_*.sigmf-data)
file(GLOB meta ${DIR}/sub_*.sigmf-meta)
if(NOT data OR NOT meta)
  message(FATAL_ERROR "the subscriber wrote no SigMF recording")
endif()
file(SIZE ${data} size)
if(NOT size EQUAL 4000000)  # 1 s at 1 MS/s, ci16
  message(FATAL_ERROR "the recording has ${size} bytes, not 4000000")
endif()
file(READ ${meta} m)
if(NOT m MATCHES "\"core:sample_rate\": *1000000" OR NOT m MATCHES "\"core:frequency\": *100000000"
   OR NOT m MATCHES "ci16_le")
  message(FATAL_ERROR "the metadata does not have the source's rate, frequency and format:\n${m}")
endif()
if(out MATCHES "gaps [1-9]")
  message(FATAL_ERROR "the recording has gaps")
endif()
message("the subscriber recorded 1 s of the stream on ${addr}")
