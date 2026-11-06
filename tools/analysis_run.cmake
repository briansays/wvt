# The Analysis window and --analyze through the real program (issue #111).
# Makes a pulsed recording with the signal generator, then:
#   MODE=cli:    --analyze KIND --analyze-out for each kind (no X server);
#                checks the report of each one and the refusals.
#   MODE=window: opens the window on each tab and takes a screenshot, so that
#                each tab draws its result. Needs an X server (Xvfb in CI);
#                prints "skipped" without one.
# -DWVT=<wvt> -DDIR=<scratch folder> -DMODE=cli|window
set(KINDS measure spectrum histogram constellation pulses correlation symbols cyclo demod)
if(MODE STREQUAL "window" AND NOT DEFINED ENV{DISPLAY})
  message("no DISPLAY: skipped")
  return()
endif()
file(REMOVE_RECURSE ${DIR})
file(MAKE_DIRECTORY ${DIR})
execute_process(COMMAND ${WVT} --no-log-file --no-config --gen pulsed -r 10M -c 1G --pri 100u --pw 10u --snr 30
                        --gen-duration 0.01 --gen-out ${DIR}/pul
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
if(rc)
  message(FATAL_ERROR "the generator failed: ${out}")
endif()
file(GLOB meta ${DIR}/pul*.sigmf-meta)
if(NOT meta)
  message(FATAL_ERROR "no recording: ${out}")
endif()

if(MODE STREQUAL "cli")
  foreach(k IN LISTS KINDS)
    execute_process(COMMAND ${WVT} --no-log-file --no-config ${meta} --analyze ${k} --analyze-out ${DIR}/${k}.csv
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
    if(rc)
      message(FATAL_ERROR "--analyze ${k} failed: ${out}")
    endif()
    file(READ ${DIR}/${k}.csv text)
    if(NOT text MATCHES "^# wvt analysis: ${k}\n")
      message(FATAL_ERROR "--analyze ${k}: no header:\n${text}")
    endif()
  endforeach()
  file(READ ${DIR}/pulses.csv text)
  if(NOT text MATCHES "# pulses=99 ")
    message(FATAL_ERROR "--analyze pulses: not 99 pulses (the first starts at sample 0):\n${text}")
  endif()
  # the template: the second pulse (100 us to 110 us)
  execute_process(COMMAND ${WVT} --no-log-file --no-config ${meta} --analyze correlation
                          --analyze-opt template=98u:112u,threshold=0.9 --analyze-out -
                  RESULT_VARIABLE rc OUTPUT_VARIABLE text ERROR_VARIABLE err)
  if(rc OR NOT text MATCHES "hits=99")
    message(FATAL_ERROR "the template search does not find the 99 pulses: ${text}${err}")
  endif()
  # refusals: a bad option, a file that exists
  execute_process(COMMAND ${WVT} --no-log-file --no-config ${meta} --analyze spectrum --analyze-opt nfft=7 --analyze-out -
                  RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
  if(NOT rc EQUAL 2 OR NOT err MATCHES "nfft wants")
    message(FATAL_ERROR "a bad --analyze-opt is not refused (${rc}): ${err}")
  endif()
  execute_process(COMMAND ${WVT} --no-log-file --no-config ${meta} --analyze spectrum --analyze-out ${DIR}/spectrum.csv
                  RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
  if(NOT rc OR NOT err MATCHES "does not replace")
    message(FATAL_ERROR "--analyze-out replaced a file (${rc}): ${err}")
  endif()
  file(REMOVE_RECURSE ${DIR})
  message("--analyze: every kind wrote its report")
  return()
endif()

foreach(k IN LISTS KINDS)
  execute_process(COMMAND ${WVT} --no-log-file --no-config --software --geometry 1400x900 ${meta} --view 0:0.002
                          --analyze ${k} --screenshot ${DIR}/${k}.ppm
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
  if(out MATCHES "cannot open X display")
    message("${out}no X server: skipped")
    return()
  endif()
  if(rc)
    message(FATAL_ERROR "the Analysis window on ${k} failed: ${out}")
  endif()
  if(NOT EXISTS ${DIR}/${k}.ppm)
    message(FATAL_ERROR "no screenshot of the ${k} tab: ${out}")
  endif()
  file(SIZE ${DIR}/${k}.ppm size)
  if(size LESS 100000)
    message(FATAL_ERROR "the screenshot of the ${k} tab is too small (${size} bytes)")
  endif()
endforeach()
file(REMOVE_RECURSE ${DIR})
message("the Analysis window drew each tab")
