#include <assert.h>
#include <stdlib.h>
#include <wchar.h>

#include "../src/windows/pty_windows_environment.h"

int main(void)
{
    const char *environment[] = {"KEY1=value1", "KEY2=value 2", "UNICODE=caf\xC3\xA9"};
    wchar_t *block = pty_windows_build_environment(environment, 3);
    assert(block != NULL);
    size_t offset = 0;
    assert(wcscmp(block + offset, L"KEY1=value1") == 0);
    offset += wcslen(block + offset) + 1;
    assert(wcscmp(block + offset, L"KEY2=value 2") == 0);
    offset += wcslen(block + offset) + 1;
    assert(wcscmp(block + offset, L"UNICODE=caf\u00E9") == 0);
    offset += wcslen(block + offset) + 1;
    assert(block[offset] == L'\0');
    free(block);

    block = pty_windows_build_environment(NULL, 0);
    assert(block != NULL);
    assert(block[0] == L'\0');
    assert(block[1] == L'\0');
    free(block);
    return 0;
}
