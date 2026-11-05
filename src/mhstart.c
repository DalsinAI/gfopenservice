/*
 * media-handler: the first code in the file, where DOS starts the handler
 * (GlobVec -1: a C handler, the startup packet waiting on its port).
 * MIT, Copyright (c) 2026 AmiCradle Team.
 */
void mh_main(void);

long mh_start(void)
{
    mh_main();
    return 0;
}
