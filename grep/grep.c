#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <stdbool.h>
#include <string.h>

#define SUCCESS 0
#define ERROR 1
#define MAX_INPUT_LINES 100
#define EXTRA_THREAD_COUNT 2
#define MAX_SEARCH_LINES 1000

HANDLE g_hSearcher;
HANDLE g_hWriter;

CRITICAL_SECTION g_InBufferCrSct;
CRITICAL_SECTION g_OutBufferCrSct;

bool working = true;
unsigned int g_InBufferCount = 0;
char **g_InBuffer = NULL;

unsigned short g_OutBufferPos = 0;
char **g_OutBuffer = NULL;

unsigned int g_MaxLinesSearchBuffer = MAX_SEARCH_LINES;

void delete_in_buffer(const char* line) {
  EnterCriticalSection(&g_InBufferCrSct);
  int i;
  for (i = 0; i < MAX_INPUT_LINES; i++) {
    if (g_InBuffer[i] == line) {
      free(g_InBuffer[i]);
      g_InBuffer[i] == NULL;
      g_InBufferCount--;
      break;
    }
  }
  assert(i != MAX_INPUT_LINES && "Fail found deleting  Indelimiter_buffer");
  LeaveCriticalSection(&g_InBufferCrSct);
}

void add_in_line_buffer(const char* delimiter_buffer) {
  EnterCriticalSection(&g_InBufferCrSct);
  assert(g_InBufferCount < MAX_INPUT_LINES && "Too many at Indelimiter_buffer delimiter_buffers");
  int i;
  for (i = 0; i < MAX_INPUT_LINES; i++) {
    if (g_InBuffer[i] == 0) {
      g_InBuffer[i] = delimiter_buffer;
      g_InBufferCount++;
      break;
    }
  }
  assert(i != MAX_INPUT_LINES && "Fail found free space to add new delimiter_buffer at Indelimiter_buffer");
  LeaveCriticalSection(&g_InBufferCrSct);
}

// only one thread use it
void push_out_delimiter_buffer(const char* delimiter_buffer) {
  EnterCriticalSection(&g_OutBufferCrSct);
  g_OutBuffer[g_OutBufferPos] = delimiter_buffer;
  g_OutBufferPos++;
  assert(g_OutBufferPos < g_MaxLinesSearchBuffer && "Too much searched delimiter_buffers");
  LeaveCriticalSection(&g_OutBufferCrSct);
}

// Main thread
void Reader(const char* path, bool scan_tail, const char* delimiter) {
  FILE* pFile;
  long lSize;
  size_t result;

  pFile = fopen(path, "r");

  if (pFile == NULL) {
    printf("Fail open file %s.", path);
  }

  // obtain file size:
  fseek(pFile, 0, SEEK_END);
  lSize = ftell(pFile);
  rewind(pFile);

  char *delimiter_buffer = (char*)malloc(sizeof(char)*strlen(delimiter) + 1);
  if (delimiter_buffer == NULL) {
    goto free_resource_with_error;
  }

  // TODO Delimiter 1 symbol
  result = fread(delimiter_buffer, 1, lSize, pFile);
  if (result != lSize) { fputs("Reading error", ); exit(3); }

  fclose(pFile);
  free(delimiter_buffer); // TODO move free from here
}

bool StartSearcher(const char* mask) {
  g_hSearcher = CreateThread(
    NULL,       // default security attributes
    0,          // default stack size
    (LPTHREAD_START_ROUTINE)Searcher,
    mask,       // thread function arguments
    0,          // default creation flags
    0); // receive thread identifier

  if (g_hSearcher == NULL) {
    printf("Fail to create search thread. Error: %d\n", GetLastError());
    return false;
  }
  return true;
}

// Thread
DWORD Searcher(LPVOID param) {
  const char* mask = (const char*)param;

  if (found) {
    Writer(delimiter_buffer);
  }
  else {
    delete_in_delimiter_buffer(delimiter_buffer);
  }
}

bool StartWriter(const char* delimiter_buffer) {
  g_hWriter = CreateThread(
    NULL,       // default security attributes
    0,          // default stack size
    (LPTHREAD_START_ROUTINE)Writer,
    delimiter_buffer,       // no thread function arguments
    0,          // default creation flags
    0); // receive thread identifier

  if (g_hWriter == NULL) {
    printf("Fail to create writer thread. Error: %d\n", GetLastError());
    return false;
  }

  return true;
}

// Thread
void Writer(LPVOID lpParam) {
  const char* delimiter_buffer = (const char*)lpParam;
  const string_size = strlen(delimiter_buffer);
  char *new_line = (int *)malloc(string_size * sizeof(int) + 1); // 0-terminated
  // C-style exception
  if (new_line == NULL) {
    goto free_resource_with_error;
  }

  // Make hard copy, prevent CoW optimization
  int i;
  for (i = 0; i < string_size; i++) {
    new_line[i] = delimiter_buffer[i];
  }
  new_line[i] = '\0';

  push_out_delimiter_buffer(new_line);
  delete_in_delimiter_buffer(delimiter_buffer);
}

bool Init() {
  bool retVal = false;

  SetLastError(0);
  InitializeCriticalSectionAndSpinCount(&g_InBufferCrSct, 0x00000400);

  DWORD error = 0;
  if ((error = GetLastError())) {
    printf("Fail init critical section for Indelimiter_buffer. Error 0x%X\n", error);
    return retVal;
  }

  SetLastError(0);
  InitializeCriticalSectionAndSpinCount(&g_OutBufferCrSct, 0x00000400);

  error = 0;
  if ((error = GetLastError())) {
    printf("Fail init critical section for Outdelimiter_buffer. Error 0x%X\n", error);
    return retVal;
  }

  g_InBuffer = (char**)calloc(MAX_INPUT_LINES, sizeof(char**));
  if (g_InBuffer == NULL) {
    goto free_resource_with_error;
  }

  g_OutBuffer = (char**)calloc(g_MaxLinesSearchBuffer, sizeof(char**));
  if (g_OutBuffer == NULL) {
    goto free_resource_with_error;
  }

}

int Destrcut() {
  DeleteCriticalSection(&g_InBufferCrSct);
  DeleteCriticalSection(&g_OutBufferCrSct);

  // Close threads
  CloseHandle(g_hWriter);
  CloseHandle(g_hSearcher);

  for (int i = 0; i < MAX_INPUT_LINES; i++) {
    free(g_InBuffer[i]);
    g_InBuffer[i] = NULL;
  }
  free(g_InBuffer);
  g_InBuffer = NULL;

  for (int i = 0; i < ; i++) {
    free(g_OutBuffer[i]);
    g_OutBuffer[i] = NULL;
  }
  free(g_OutBuffer);
  g_OutBuffer = NULL;

  return SUCCESS;
}

int main(int argc, char** argv) {
  int retVal = SUCCESS;

  char help_message[] = "It is grep function help message\
    to run program : \
    grep.exe[file_path][mask][max_delimiter_buffers][scan_tail][separator]\
    file_path – mandatory, path to file\
    mask      – mandatory, search mask posssible to use[*and ? ] \
    max_delimiter_buffers – optional if no more parameters, delimiter_buffers to search, max = 1000 \
    scan_tail – optional if no more parameters, search from end, 0 - false, any other true, default false \
    separator – optional, string separator, for example <br>.Default '\\n' \n";

  char *file_path = NULL;
  char *mask = NULL;
  bool scan_tail = false;
  const char default_separator[] = "\n";
  char *separator = default_separator;

  switch (argc) {
  case 0:
  case 1:
  case 2: { // not enough params
    printf("%s", help_message);
    return ERROR;
  }
  case 6: // delimiter
    separator = argv[5];
  case 5: {// scan_tail
    scan_tail = (argv[4][0] == '0') ? false : true;
  }
  case 4: {// max_delimiter_buffers
    g_MaxLinesSearchBuffer = (unsigned short)atoi(argv[3]);
    if (g_MaxLinesSearchBuffer > MAX_SEARCH_LINES) {
      printf("Incorrect value of max delimiter_buffers\n");
      return ERROR;
    }
  }
  case 3: { // mask and path
    if (file_path[strlen(file_path) - 1] == '\\') {
      printf("Incorrect file path. File path must contain file name\n");
      return ERROR;
    }
    file_path = argv[1];
    mask = argv[2];
    break;
  }
  }

  if (!Init()) {
    return ERROR;
  }

  // searching start here
  Reader();

  // Wait for Searcher
  WaitForMultipleObjects(g_hSearcher, INFINITE);
  // Wait for Writer
  WaitForMultipleObjects(g_hWriter, INFINITE);

  goto free_resource;

free_resource_with_error:
  printf("Fail to allocate memory\n");

free_resource:
  retVal = Destrcut();
  return retVal;
}