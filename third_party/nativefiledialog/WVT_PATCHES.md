# Native File Dialog in wvt

Upstream: https://github.com/mlabbe/nativefiledialog, commit 67345b8
(2019-09-30), zlib license (LICENSE). Only the files wvt builds are here:
the shared code, the Cocoa back end (macOS) and the zenity back end (Linux;
it runs the `zenity` program, so nothing is needed at build time). wvt's own
CMakeLists.txt builds them (upstream uses premake).

wvt calls the library only through src/common/file_dialog.{h,cpp}.

## Changes from upstream

1. nfd_zenity.c: a cancel made zenity print nothing, and the callers then
   wrote one byte before a malloc(0) buffer. An empty answer is now a cancel,
   an exit code other than 0 or 1 (no display, for example) is an error, and
   NFD_OpenDialogMultiple returns NFD_CANCEL (not NFD_ERROR) on a cancel.
2. simple_exec.h: when zenity is not installed, the forked child called
   exit(), which runs the parent's atexit handlers and static destructors in
   a copy of a multithreaded process. It now calls _exit().
3. nfd_cocoa.m: the default path was always used as a folder. A path that is
   not a folder now opens the folder it is in, and a save dialog starts with
   the path's file name.

The exact diff:

```diff
diff --git a/src/nfd_cocoa.m b/src/nfd_cocoa.m
index 776152d..f0681cc 100644
--- a/src/nfd_cocoa.m
+++ b/src/nfd_cocoa.m
@@ -63,6 +63,15 @@ static void SetDefaultPath( NSSavePanel *dialog, const nfdchar_t *defaultPath )
         return;
 
     NSString *defaultPathString = [NSString stringWithUTF8String: defaultPath];
+    /* wvt patch: a path that is not a folder opens the folder it is in, and a
+       save dialog starts with its file name. */
+    BOOL isDir = NO;
+    if ( ![[NSFileManager defaultManager] fileExistsAtPath:defaultPathString isDirectory:&isDir] || !isDir )
+    {
+        if ( ![dialog isKindOfClass:[NSOpenPanel class]] )
+            [dialog setNameFieldStringValue:[defaultPathString lastPathComponent]];
+        defaultPathString = [defaultPathString stringByDeletingLastPathComponent];
+    }
     NSURL *url = [NSURL fileURLWithPath:defaultPathString isDirectory:YES];
     [dialog setDirectoryURL:url];    
 }
diff --git a/src/nfd_zenity.c b/src/nfd_zenity.c
index 5f93133..06c4025 100644
--- a/src/nfd_zenity.c
+++ b/src/nfd_zenity.c
@@ -125,6 +125,23 @@ static nfdresult_t ZenityCommon(char** command, int commandLen, const char* defa
     {
         if(exitCode == 1)
             result = NFD_CANCEL;
+        /* wvt patch: other exit codes (no display, a timeout) are errors, and
+           an empty answer is a cancel, not a path. */
+        else if(exitCode != 0)
+        {
+            NFDi_SetError("zenity failed");
+            result = NFD_ERROR;
+        }
+        else if(*stdOut == NULL || (*stdOut)[0] == '\0' || (*stdOut)[0] == '\n')
+            result = NFD_CANCEL;
+    }
+
+    /* wvt patch: the callers copy the output only when there is a path; an
+       empty output made them write before the start of a malloc(0) buffer. */
+    if(result != NFD_OKAY && *stdOut != NULL)
+    {
+        free(*stdOut);
+        *stdOut = NULL;
     }
 
     return result;
@@ -235,10 +252,7 @@ nfdresult_t NFD_OpenDialogMultiple( const nfdchar_t *filterList,
 
         free(stdOut);
     }
-    else
-    {
-        result = NFD_ERROR;
-    }
+    /* wvt patch: no output keeps the result (a cancel stays a cancel). */
 
     return result;
 }
diff --git a/src/simple_exec.h b/src/simple_exec.h
index 6308dfe..ba49e9e 100644
--- a/src/simple_exec.h
+++ b/src/simple_exec.h
@@ -104,7 +104,7 @@ int runCommandArray(char** stdOut, int* stdOutByteCount, int* returnCode, int in
             close(parentToChild[READ_FD]);
             close(childToParent[WRITE_FD]);
 
-            exit(0);
+            _exit(0); /* wvt patch: not exit(): no atexit handlers or static destructors in the forked child */
         }
 
 
```
