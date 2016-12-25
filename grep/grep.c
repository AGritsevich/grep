#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

#define SUCCESS 0
#define GREP_ERROR 1
#define MAX_INPUT_LINES 100
#define EXTRA_THREAD_COUNT 2
#define MAX_SEARCH_LINES 1000

HANDLE g_hSearcher;
HANDLE g_hWriter;

CRITICAL_SECTION g_InBufferCrSct;
CRITICAL_SECTION g_OutBufferCrSct;

CONDITION_VARIABLE g_InBufferFull;
CONDITION_VARIABLE g_InBufferNotEmpty;

bool working = true;
unsigned int g_InBufferCount = 0;
char **g_InBuffer = NULL;

unsigned short g_OutBufferPos = 0;
char **g_OutBuffer = NULL;

unsigned int g_MaxLinesSearchBuffer = MAX_SEARCH_LINES;

int Destruct();

void delete_in_line(const char* line) {
  EnterCriticalSection(&g_InBufferCrSct);
  int i;
  for (i = 0; i < MAX_INPUT_LINES; i++) {
    if (g_InBuffer[i] == line) {
      free(g_InBuffer[i]);
      g_InBuffer[i] = NULL;
      g_InBufferCount--;
      break;
    }
  }
  assert(i != MAX_INPUT_LINES && "Fail found deleting  Indelimiter_buffer");
  LeaveCriticalSection(&g_InBufferCrSct);
  WakeConditionVariable(&g_InBufferFull);
}

void add_in_line(const char* line) {
  EnterCriticalSection(&g_InBufferCrSct);
  while (g_InBufferCount == MAX_INPUT_LINES)
  {
    // Buffer is full - sleep while searcher delete at least one
    SleepConditionVariableCS(&g_InBufferFull, &g_InBufferCrSct, INFINITE);
  }

  assert(g_InBufferCount < MAX_INPUT_LINES && "Too many at Indelimiter_buffer delimiter_buffers");
  int i;
  for (i = 0; i < MAX_INPUT_LINES; i++) {
    if (g_InBuffer[i] == NULL) {
      (const char*)g_InBuffer[i] = line;
      g_InBufferCount++;
      break;
    }
  }
  assert(i != MAX_INPUT_LINES && "Fail found free space to add new delimiter_buffer at Indelimiter_buffer");
  LeaveCriticalSection(&g_InBufferCrSct);
}

// no need lock it mustn't change data. Read access here free
char* GetInString() {
  for (int i = 0; i < MAX_INPUT_LINES; i++) {
    if (g_InBuffer[i] != NULL) {
      return g_InBuffer[i];
    }
  }
  return NULL;
}

// only one thread use it
void push_out_delimiter_buffer(const char* line) {
  EnterCriticalSection(&g_OutBufferCrSct);
  (const char*)g_OutBuffer[g_OutBufferPos] = line;
  g_OutBufferPos++;
  assert(g_OutBufferPos < g_MaxLinesSearchBuffer && "Too much searched delimiter_buffers");
  LeaveCriticalSection(&g_OutBufferCrSct);
}

// Main thread
void Reader(const char* path, bool scan_tail, const char* delimiter) {
  FILE* pFile = NULL;

  if (0 == fopen_s(&pFile, path, "r")) {
    printf("Fail open file %s.", path);
  }

  // obtain file size:
  long file_size;
  fseek(pFile, 0, SEEK_END);
  file_size = ftell(pFile);
  rewind(pFile);

  const size_t delimiter_size = sizeof(char)*strlen(delimiter) - 1; // without 0-end
  char *delimiter_buffer = (char*)calloc(delimiter_size, sizeof(char));
  if (delimiter_buffer == NULL) {
    goto free_resource_with_error;
  }

  // I don't now how easy in plain C read until delimiter
  size_t elements_read;
  const unsigned char one_element = 1;
  unsigned long previous_pos = (!scan_tail) ? 0 : file_size;
  unsigned long line_size = 0; 

  while (elements_read != one_element) {
    unsigned long current_pos = previous_pos;
    do {
      elements_read = fread(delimiter_buffer, delimiter_size, one_element, pFile);
      current_pos += (!scan_tail) ? 1 : -1;
      fseek(pFile, current_pos, SEEK_SET);
    } while (0 != strcmp(delimiter_buffer, delimiter) || elements_read != one_element);

    // now get position delimiter or end of file
    // read line from begin to here
    unsigned long end_line_pos = ftell(pFile);
    line_size = end_line_pos - previous_pos;
    previous_pos = end_line_pos;

    char *line = (char*)calloc(line_size + 1, sizeof(char));
    if (line == NULL) {
      goto free_resource_with_error;
    }

    fseek(pFile, previous_pos, SEEK_SET);
    fread(line, line_size, 1, pFile);
    add_in_line(line);
  }

  fclose(pFile);
  free(delimiter_buffer);
  return;

// C-Style exception
free_resource_with_error:
  // Of cause we can try continue working,
  // allocate memory small parts or some thing else
  // but I don't see why do it in current case
  printf("Fail to allocate memory in Reader for %u bytes\n", (unsigned int)((line_size + 1) * sizeof(char)));
  fclose(pFile);
  free(delimiter_buffer);
  Destruct();
  exit(GREP_ERROR);
}

// Thread
DWORD Writer(LPVOID lpParam) {
  const char* in_line = (const char*)lpParam;
  const size_t string_size = strlen(in_line);
  char *new_line = (char *)calloc(string_size + 1, sizeof(char)); // 0-terminated
                                                                 // C-style exception
  if (new_line == NULL) {
    goto free_resource_with_error;
  }

  // Make hard copy, prevent CoW optimization
  int i;
  for (i = 0; i < string_size; i++) {
    new_line[i] = in_line[i];
  }
  new_line[i] = '\0';

  add_in_line(new_line);
  delete_in_line(in_line);

  return 0;

  // C-Style exception
free_resource_with_error:
  // TODO: Sync output print
  printf("Fail to allocate memory in Writer, to %u bytes\n", (unsigned int)((string_size + 1) * sizeof(char)));
  Destruct();
  exit(GREP_ERROR);
}

void StartWriter(char* line) {
  g_hWriter = CreateThread(
    NULL,       // default security attributes
    0,          // default stack size
    (LPTHREAD_START_ROUTINE)Writer,
    line,       // function arguments
    0,          // default creation flags
    0); // receive thread identifier

  if (g_hWriter == NULL) {
    printf("Fail to create writer thread. Error: %d\n", GetLastError());
    return;
  }
}

// Thread
// On C no regex lib for windows,
// Manually do it quite difficult 
// So, it doesn't support * and ?
DWORD Searcher(LPVOID param) {
  const char* full_mask = (const char*)param;
  const size_t full_mask_size = strlen(full_mask);

  while (working) {
    char *line = NULL;
    while (NULL == (line = GetInString())) {
      SleepConditionVariableCS(&g_InBufferNotEmpty, &g_InBufferCrSct, INFINITE);
    }

    const size_t line_size = strlen(line);
    bool found = false;
    // Search exact match
    for (size_t i = 0; i < line_size; i++) {
      if (0 == strcmp((char const*)line[i], (char const*)full_mask)) {
        found = true;
        break;
      }
    }

    if (found) {
      StartWriter(line);
    }
    else {
      delete_in_line(line);
    }
  }

  return 0;
}

bool StartSearcher(char* mask) {
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

  InitializeConditionVariable(&g_InBufferFull);
  InitializeConditionVariable(&g_InBufferNotEmpty);

  g_InBuffer = (char**)calloc(MAX_INPUT_LINES, sizeof(char**));
  if (g_InBuffer == NULL) {
    goto free_resource_with_error;
  }

  g_OutBuffer = (char**)calloc(g_MaxLinesSearchBuffer, sizeof(char**));
  if (g_OutBuffer == NULL) {
    goto free_resource_with_error;
  }

  return;

  // C-Style exception
free_resource_with_error:
  // TODO: Sync output print
  printf("Fail to allocate memory in Writer, to %d bytes or \n", 
    ((g_MaxLinesSearchBuffer + 1) * sizeof(char)), 
    ((MAX_INPUT_LINES + 1) * sizeof(char)));
  Destruct();
  exit(GREP_ERROR);
}

int Destruct() {
  WakeAllConditionVariable(&g_InBufferNotEmpty);
  WakeAllConditionVariable(&g_InBufferFull);
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

  for (unsigned int i = 0; i < g_MaxLinesSearchBuffer; i++) {
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
    \rto run program : \
    \r grep.exe[file_path][mask][max_delimiter_buffers][scan_tail][separator]\
    \r file_path – mandatory, path to file\
    \r mask      – mandatory, search mask posssible to use[*and ? ] \
    \r max_delimiter_buffers – optional if no more parameters, delimiter_buffers to search, max = 1000 \
    \r scan_tail – optional if no more parameters, search from end, 0 - false, any other true, default false \
    \r separator – optional, string separator, for example <br>.Default '\\n' \n";

  char *file_path = NULL;
  char *mask = NULL;
  bool scan_tail = false;
  const char default_separator[] = "\n";
  char *delimiter = (char*)default_separator;

  switch (argc) {
  case 0:
  case 1:
  case 2: { // not enough params
    printf("%s", help_message);
    return GREP_ERROR;
  }
  case 6: // delimiter
    delimiter = argv[5];
  case 5: {// scan_tail
    scan_tail = (argv[4][0] == '0') ? false : true;
  }
  case 4: {// max_delimiter_buffers
    g_MaxLinesSearchBuffer = (unsigned short)atoi(argv[3]);
    if (g_MaxLinesSearchBuffer > MAX_SEARCH_LINES) {
      printf("Incorrect value of max delimiter_buffers\n");
      return GREP_ERROR;
    }
  }
  case 3: { // mask and path
    if (file_path[strlen(file_path) - 1] == '\\') {
      printf("Incorrect file path. File path must contain file name\n");
      return GREP_ERROR;
    }
    file_path = argv[1];
    mask = argv[2];
    break;
  }
  }

  if (!Init()) {
    return GREP_ERROR;
  }

  // searching start here
  Reader(file_path, scan_tail, delimiter);

  // Wait for Searcher
  WaitForSingleObject(g_hSearcher, INFINITE);
  // Wait for Writer
  WaitForSingleObject(g_hWriter, INFINITE);

  retVal = Destruct();
  return retVal;
}