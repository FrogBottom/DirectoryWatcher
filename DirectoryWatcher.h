#pragma once

/*
DirectoryWatcher is Windows-only library for monitoring file/directory changes, intended to be relatively
lightweight, mostly for use in game engines and such. If you don't particulary care how it works, and just
want to get started, the basic way to use it is:
    1. Call Initialize().
    2. Add one or more directories with AddDirectory(). You can optionally monitor subdirectories too.
    3. At some point in your application update loop, call TryGetNextChange() until it returns false.
    4. Call ShutDown() when you are finished, or feel free not to if the application is closing anyway.

For example, if an application had functions called AppStart(), AppLoop(), and AppEnd(), a valid use would be:

DirectoryWatcher watcher = {};
void AppStart()
{
    watcher.Initialize();
    watcher.AddDirectory("data/assets");
}

void AppLoop()
{
    DirectoryWatcher::FileChange change = {};
    while (watcher.TryGetNextChange(&change))
    {
        // Do whatever processing you need to in here.
    }
}
void AppEnd()
{
    watcher.ShutDown();
}

A few notes about the implementation:

This library uses the ReadDirectoryChangesExW function, overlapped I/O and a second thread which runs
completion routines for each watched directory. The watcher thread is started during Initialize(), and
sleeps until a change is received. Other reasonable approaches might be to use I/O completion ports or
the FindFirstChangeNotification/FindNextChangeNotification API, but this approach has a few advantages:
    1. Unlike the FindFirst/FindNext API, we don't need to keep track of the whole directory tree, and
       we don't need to iterate through it to figure out what changed.
    2. Unlike I/O completion ports, we don't need an additional thread for every monitored directory,
       just a single watcher thread.

One disadvantage of this approach is that it requires a statically sized buffer to hold the changes
(we actually use two buffers and swap back and forth). You can change the buffer size when adding a
directory to be monitored. If enough changes happen at one time to fill the buffer, then the call fails,
and we would need to track/scan the directory manually to see what changed - which we want to avoid!


A note about MAX_PATH:

This is one of a few cases where MAX_PATH is actually appropriate. Typically we would not
want to use it, since a Windows path string is allowed to be longer in some circumstances,
up to *approximately* (eye roll) 32767 "wide" characters, depending on a few factors.

However, ReadDirectoryChangesW always gives us a relative path, and those *are* restricted to MAX_PATH
"wide" characters. A single "wide" (UTF-16 ish) code unit can take up to 3 bytes to represent in UTF-8,
so after we convert to UTF-8, the maximum relative path length is just under (MAX_PATH * 3) bytes,
including a null terminator.

Some alterations you may want to make:

The main and watcher threads communicate via a thread-safe queue, and you may have your own data structure
that you would prefer to use instead. As long as the watcher thread can push to the queue and the main
thread can pop elements off of it in a thread-safe way, it should work, with one caveat. Because renaming
a file produces both a "Rename From" and  "Rename To" event, it is possible in rare cases for these events
to be separated. The queue Pop() function checks for this. If the queue has one element in it and that
element is a "Rename From" event, it returns nothing. You could choose to replicate this behavior if you
swap in your own queue implementation, or you could make the calling code handle that case.

The event queue contains FileChange structs, which are a fixed size. This tends to waste a lot of space for
the file path. At the cost of some API complexity, you could use a similar approach to ReadDirectoryChangesW
and iterate through a buffer where each element's size depends on the path length. This could save a lot of
space and prevent needless copying.

The library has two C standard library dependencies, assert.h for assert(), and stdlib.h for malloc()
and free(). You could replace or remove the asserts, and replace allocations with your own scheme.

If you know at compile time how large of a change buffer you want, which directories you are monitoring, and
provided you are willing to use a fixed-size queue, then you could avoid doing any dynamic allocations at all.
To contextualize the buffer sizes:
    - The library allocates space for two change buffers per monitored directory. One buffer element uses
      ~88 bytes plus the size of the relative file path (in UTF-16 ish). If it fills up, we have to give up
      and return an error.
    - The event queue uses 840 bytes per change, since the FileChange struct is a fixed size and needs to
      have space for a maximum length relative path.
*/

#include <Windows.h>

#define _CRT_SECURE_NO_WARNINGS

#include <assert.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;

struct DirectoryWatcher
{
    enum struct EFileAction
    {
        None = 0,
        Added,
        Removed,
        Modified,
        RenamedFrom,
        RenamedTo,
        TooManyChanges, // Note(Frog): This can happen if many changes happen at once and the change buffer is small.
        Count
    };

    struct FileChange
    {
        char path[MAX_PATH * 3];
        s32 path_length;

        EFileAction action;

        u64 creation_time;
        u64 modification_time;
        u64 change_time;
        u64 access_time;
        u64 size;
        u32 attributes;
        bool is_directory;
    };

    //Initialize the directory watcher, creating a (sleeping) thread to wait for changes.
    void Initialize();
    // Destroys the directory watcher. This cancels any I/O operations and blocks until the watcher thread completes.
    void ShutDown();
    // Adds a directory to monitor for changes, optionally monitoring all subdirectories as well.
    bool AddDirectory(const char* directory, bool is_recursive = true, s32 change_buffer_size = 32768);
    // Gets the next change which occured since the last call to this function, or nothing if there are no more changes.
    bool TryGetNextChange(FileChange* out_change);

private:

    struct ReadChangesRequest
    {
        DirectoryWatcher* watcher; // Parent directory watcher that made the request.
        u8* buffers;
        char16_t* path;
        s32 buffer_size;
        s32 path_length;

        OVERLAPPED overlapped;

        s32 buffer_index;
        bool is_recursive;
        void* directory;
        ReadChangesRequest* next; // Link to the next request in the list, if any.
    };

    struct ThreadSafeQueue
    {
        static const s32 InitialCapacity = 16;
        static const s32 GrowRate = 2; // Multiplier for the new capacity if we need to grow the queue.

        s32 lock = 0;
        FileChange* data = 0;
        s32 capacity = 0;
        s32 count = 0;
        s32 front_index = 0;

        void Create();
        void Push(FileChange element);
        bool Pop(FileChange* element);
        void Destroy();

        inline void Lock();
        inline void Unlock();

        private:
        void Grow();
    };

    void ProcessNotification(u8* buffer, const char16_t* directory_path, s32 directory_path_length);
    void BeginRead(ReadChangesRequest* request);

    static u32 __stdcall ThreadProc(void* arg);
    static void __stdcall ThreadAddDirectoryProc(u64 arg);
    static void __stdcall NotificationCompletion(u32 error_code, u32 bytes_transferred, OVERLAPPED* overlapped);

    ThreadSafeQueue queue = {};
    ReadChangesRequest* requests = 0;
    void* thread_handle = 0;
    bool should_terminate = false;
    u32 outstanding_request_count = 0; // NOTE(Frog): This is incremented/decremented atomically on different threads.
};
