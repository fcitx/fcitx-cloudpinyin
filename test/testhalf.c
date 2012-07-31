#include "parse.h"

#include <assert.h>

int main(int argc, char* argv[])
{
    char* result = MapSogouStringToHalf("ＡＢＣＤ");
    printf("%s\n", result);
    assert(strcmp(result, "abcd") == 0);

    free(result);

    result = MapSogouStringToHalf("我ａ测ｂ你ＣＤ的");
    printf("%s\n", result);
    assert(strcmp(result, "我a测b你cd的") == 0);

    free(result);
    return 0;
}
