# Session round trip through the real program (issue #76): open a test
# recording with options and write a session, open that session and write
# it again; the two files must be the same, so every value the App saves
# is also put back. Needs an X server (Xvfb in CI); prints "skipped"
# without one. -DWVT=<wvt> -DIQGEN=<iqgen> -DDIR=<scratch folder>
if(NOT DEFINED ENV{DISPLAY})
  message("no DISPLAY: skipped")
  return()
endif()
file(REMOVE_RECURSE ${DIR})
file(MAKE_DIRECTORY ${DIR})
execute_process(COMMAND ${IQGEN} ${DIR}/rec.sigmf-data --fmt ci16 RESULT_VARIABLE rc OUTPUT_QUIET)
if(rc)
  message(FATAL_ERROR "iqgen failed")
endif()
execute_process(COMMAND ${WVT} --no-log-file --software ${DIR}/rec.sigmf-meta --view 0.5:0.75 --chan 150k:30k
                        --segment 0.25:1.75 --session-out ${DIR}/a.wvtsession.json
                RESULT_VARIABLE rc ERROR_VARIABLE out)
if(out MATCHES "cannot open X display")
  message("${out}no X server: skipped")
  return()
endif()
if(rc)
  message(FATAL_ERROR "first --session-out failed: ${out}")
endif()
execute_process(COMMAND ${WVT} --no-log-file --software ${DIR}/a.wvtsession.json --session-out ${DIR}/b.wvtsession.json
                RESULT_VARIABLE rc ERROR_VARIABLE out)
if(rc)
  message(FATAL_ERROR "second --session-out failed: ${out}")
endif()
file(READ ${DIR}/a.wvtsession.json a)
file(READ ${DIR}/b.wvtsession.json b)
if(NOT a STREQUAL b)
  message(FATAL_ERROR "the session changed when opened and saved again:\n${a}\n---\n${b}")
endif()
if(NOT a MATCHES "\"offset_hz\": 150000" OR NOT a MATCHES "\"t0_s\": 0.5" OR NOT a MATCHES "\"segment_s\": \\[0.25, 1.75\\]")
  message(FATAL_ERROR "the session does not have the options:\n${a}")
endif()
file(REMOVE_RECURSE ${DIR})
message("session round trip: the same file")
