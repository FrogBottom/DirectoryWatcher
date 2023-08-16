#include "DirectoryWatcher.h"

void DirectoryWatcher::Initialize()
{
    requests = 0;
    should_terminate = false;
    outstanding_request_count = 0;
    queue.Create();
    thread_handle = CreateThread(0, 0, (LPTHREAD_START_ROUTINE)DirectoryWatcher::ThreadProc, this, 0, 0);
}

void DirectoryWatcher::ShutDown()
{
    should_terminate = true;

    ReadChangesRequest* current = requests;
    while (current)
    {
        ReadChangesRequest* request = current;
        CancelIo(request->directory);
        CloseHandle(request->directory);
        current = current->next;
    }
    CloseHandle(thread_handle);
    queue.Destroy();
}

bool DirectoryWatcher::AddDirectory(const char* directory, bool is_recursive, s32 change_buffer_size)
{
    assert(thread_handle && directory && change_buffer_size > 0);

     // Get the number of bytes in the converted string, including the null terminator, so we know
     // how big of a buffer we will need.
    s32 path_count = MultiByteToWideChar(CP_UTF8, 0, directory, -1, 0, 0);

    // Allocate space for the request struct, two buffers, and the directory path.
    u8* memory = (u8*)malloc(sizeof(ReadChangesRequest) + (2 * change_buffer_size) + (2 * path_count));
    assert(memory && path_count > 0); // Make sure we got our memory, and that the path is a valid string.

    ReadChangesRequest* request = (ReadChangesRequest*)memory;
    request->buffers = memory + sizeof(ReadChangesRequest);
    request->buffer_size = change_buffer_size;

    request->path = (char16_t*)(request->buffers + (2 * change_buffer_size));
    request->path_length = MultiByteToWideChar(CP_UTF8, 0, directory, -1, (LPWSTR)request->path, path_count) - 1;

    // Go ahead and open the directory handle.
    u32 mode = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    u32 flags = FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED;
    request->directory = CreateFileW((LPWSTR)request->path, FILE_LIST_DIRECTORY, mode, 0, OPEN_EXISTING, flags, 0);

    // Fill in the rest of the data, or free the buffer if we didn't get a valid directory handle.
    if (request->directory != (void*)-1)
    {
        request->watcher = this;
        request->buffer_index = 0;
        request->is_recursive = is_recursive;
        request->next = 0;

        // NOTE(Frog): We can pack our request pointer into the event handle, since ReadDirectoryChangesW doesn't
        // touch it when using a completion routine.
        request->overlapped = {};
        request->overlapped.hEvent = request;

        // Append this request to the list.
        ReadChangesRequest* last = requests;
        if (last)
        {
            while (last->next) last = last->next;
            last->next = request;
        }
        else requests = request;

        QueueUserAPC(DirectoryWatcher::ThreadAddDirectoryProc, thread_handle, (u64)request);
        return true;
    }
    else
    {
        free(request);
        return false;
    }
}

bool DirectoryWatcher::TryGetNextChange(FileChange* out_change) {return queue.Pop(out_change);}

u32 __stdcall DirectoryWatcher::ThreadProc(void* arg)
{
    DirectoryWatcher* watcher = (DirectoryWatcher*)arg;
    while (watcher->outstanding_request_count || !watcher->should_terminate) SleepEx((u32)-1, true);
    return 0;
}

void __stdcall DirectoryWatcher::ThreadAddDirectoryProc(u64 arg)
{
    DirectoryWatcher::ReadChangesRequest* request = (DirectoryWatcher::ReadChangesRequest*)arg;
    DirectoryWatcher* watcher = request->watcher;

    InterlockedIncrement(&watcher->outstanding_request_count);
    watcher->BeginRead(request);
}

void __stdcall DirectoryWatcher::NotificationCompletion(u32 error_code, u32 bytes_transferred, OVERLAPPED* overlapped)
{
    DirectoryWatcher::ReadChangesRequest* request = (DirectoryWatcher::ReadChangesRequest*)overlapped->hEvent;
    DirectoryWatcher* watcher = request->watcher;

    // This occurs on shutdown, close the request and return.
    if (error_code == ERROR_OPERATION_ABORTED || watcher->should_terminate)
    {
        InterlockedDecrement(&watcher->outstanding_request_count);
        free(request);
        return;
    }
    else if (error_code) assert(false);

    bool did_overflow = (!error_code && !bytes_transferred);

    // Cycle between the two change buffers, immediately kick off another read request (so we don't miss anything),
    // and process the change buffer we just received.
    u8* buffer = request->buffers + (request->buffer_size * (request->buffer_index ^ 1));
    watcher->BeginRead(request);
    watcher->ProcessNotification((did_overflow) ? 0 : buffer, request->path, request->path_length);
}

void DirectoryWatcher::ProcessNotification(u8* buffer, const char16_t* directory_path, s32 directory_path_length)
{
    // NOTE(Frog): If we get a null buffer, it means that it overflowed. We will enqueue an error and return.
    if (!buffer)
    {
        FileChange change = {};
        change.action = EFileAction::TooManyChanges;
        change.is_directory = true;
        change.path_length = WideCharToMultiByte(CP_UTF8, 0, (LPCWSTR)directory_path, directory_path_length, change.path, MAX_PATH * 3, 0, 0);
        change.path[change.path_length] = L'\0';
        queue.Push(change);
        return;
    }

    // The buffer has been filled with notify event structs, which have a variable size depending on the path length.
    // Each event stores the offset of the next one, so we will process them one at a time.
    FILE_NOTIFY_EXTENDED_INFORMATION* event = 0;
    s32 offset = 0;
    do
    {
        event = (FILE_NOTIFY_EXTENDED_INFORMATION*)(buffer + offset);
        offset += event->NextEntryOffset;

        s32 combined_path_length = directory_path_length;
        // Append the "filename" (in reality the path from the monitored directory) to the directory path.
        char16_t combined_path[MAX_PATH]; // NOTE(Frog): This is a relative path, which is why MAX_PATH is safe here.
        CopyMemory(combined_path, directory_path, combined_path_length * 2);
        if (combined_path[combined_path_length - 1] != L'\\') combined_path[combined_path_length++] = L'\\';
        CopyMemory(combined_path + combined_path_length, event->FileName, event->FileNameLength);
        combined_path_length += (event->FileNameLength / 2);
        combined_path[combined_path_length] = L'\0';

        // Figure out what change occured to the file/directory.
        EFileAction action;
        switch (event->Action)
        {
            case FILE_ACTION_ADDED: action = EFileAction::Added; break;
            case FILE_ACTION_REMOVED: action = EFileAction::Removed; break;
            case FILE_ACTION_MODIFIED: action = EFileAction::Modified; break;
            case FILE_ACTION_RENAMED_OLD_NAME: action = EFileAction::RenamedFrom; break;
            case FILE_ACTION_RENAMED_NEW_NAME: action = EFileAction::RenamedTo; break;
            default: action = EFileAction::None; break;
        }

        // Fill in the extended info about the file.
        FileChange change = {};
        change.creation_time = event->CreationTime.QuadPart;
        change.modification_time = event->LastModificationTime.QuadPart;
        change.change_time = event->LastChangeTime.QuadPart;
        change.access_time = event->LastAccessTime.QuadPart;
        change.size = event->FileSize.QuadPart;
        change.attributes = event->FileAttributes;

        change.action = action;
        change.is_directory = (change.attributes & FILE_ATTRIBUTE_DIRECTORY);
        change.path_length = WideCharToMultiByte(CP_UTF8, 0, (LPCWSTR)combined_path, combined_path_length + 1, change.path, MAX_PATH * 3, 0, 0) - 1;
        queue.Push(change);
    } while (event->NextEntryOffset);
}

void DirectoryWatcher::BeginRead(ReadChangesRequest* request)
{
    u32 filters = FILE_NOTIFY_CHANGE_CREATION | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME;
    u8* buffer = request->buffers + (request->buffer_size * request->buffer_index);
    request->buffer_index ^= 1; // Toggle between buffer index 0 and 1 for every notification.
    ReadDirectoryChangesExW(request->directory, buffer, request->buffer_size, request->is_recursive, filters, 0, &request->overlapped,
                            (LPOVERLAPPED_COMPLETION_ROUTINE)DirectoryWatcher::NotificationCompletion, ReadDirectoryNotifyExtendedInformation);
}

void DirectoryWatcher::ThreadSafeQueue::Create()
{
    lock = 0;
    data = (FileChange*)malloc(sizeof(FileChange) * InitialCapacity);
    capacity = InitialCapacity;
    count = 0;
    front_index = 0;
}

void DirectoryWatcher::ThreadSafeQueue::Push(FileChange element)
{
    Lock();
    if (count == capacity) Grow();
    s32 back_index = (front_index + count) % capacity;
    data[back_index] = element;
    count += 1;
    Unlock();
}

bool DirectoryWatcher::ThreadSafeQueue::Pop(FileChange* element)
{
    assert(element);
    bool result = false;

    Lock();
    // Note(Frog): Since a rename sends two events, in very rare cases the processing thread might poll in
    // between the "rename from" and "rename to" events being added to the queue. In that case, we will
    // not return the "rename from" event until the matching event comes through.
    bool has_split_rename = (count == 1 && (data[front_index].action == EFileAction::RenamedFrom));
    if (count && !has_split_rename)
    {

        *element = data[front_index];
        count -= 1;
        front_index = (front_index + 1) % capacity;
        result = true;
    }
    Unlock();

    return result;
}

void DirectoryWatcher::ThreadSafeQueue::Destroy()
{
    Lock();
    free(data);
    data = 0;
    count = 0;
    capacity = 0;
    front_index = 0;
    Unlock();
}

void DirectoryWatcher::ThreadSafeQueue::Grow()
{
    assert(data && capacity);

    s32 new_capacity = capacity * GrowRate;
    FileChange* new_data = (FileChange*)malloc(sizeof(FileChange) * new_capacity);

    if (front_index + count > capacity)
    {
        s32 block1_count = (capacity - front_index);
        s32 block2_count = count - block1_count;
        CopyMemory(new_data, data + front_index, block1_count * sizeof(FileChange));
        CopyMemory(new_data + block1_count, data, block2_count * sizeof(FileChange));
    }
    else CopyMemory(new_data, data + front_index, count * sizeof(FileChange));

    free(data);
    data = new_data;
    capacity = new_capacity;
    front_index = 0;
}

void DirectoryWatcher::ThreadSafeQueue::Lock() {while (InterlockedExchange((LPLONG)&lock, 1) == 1) {/*spin!*/};}
void DirectoryWatcher::ThreadSafeQueue::Unlock() {InterlockedExchange((LPLONG)&lock, 0);}
