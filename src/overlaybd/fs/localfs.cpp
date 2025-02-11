/*
   Copyright The Overlaybd Authors

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/
#include "localfs.h"
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/uio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/ioctl.h>
#include <dirent.h>
#ifdef __linux__
#include <linux/fs.h>
#include <sys/vfs.h>
#include <sys/xattr.h>
#include <linux/fiemap.h>
#ifndef FALLOC_FL_KEEP_SIZE
#define FALLOC_FL_KEEP_SIZE 0x01 /* default is extend size */
#endif
#ifndef FALLOC_FL_PUNCH_HOLE
#define FALLOC_FL_PUNCH_HOLE 0x02 /* de-allocates range */
#endif
#endif //__linux__
#include "virtual-file.h"
#include "fiemap.h"
#include "subfs.h"
#include "../photon/syncio/aio-wrapper.h"
#include "../alog.h"
#include "../photon/thread.h"
using namespace photon;

// UN-interrupted syscall
#define UISysCall(call)                                                                            \
    ([&]() {                                                                                       \
        while (true) {                                                                             \
            auto ret = (call);                                                                     \
            if (ret >= 0)                                                                          \
                return ret;                                                                        \
            auto e = errno;                                                                        \
            thread_usleep(10 * 1000);                                                              \
            if (e == EINTR) {                                                                      \
                continue;                                                                          \
            } else {                                                                               \
                errno = e;                                                                         \
                return ret;                                                                        \
            }                                                                                      \
        }                                                                                          \
    })()

#undef UISysCall // no longer needed
#define UISysCall(call) call

namespace FileSystem {
class BaseFileAdaptor : public VirtualFile, public IFileXAttr {
public:
    int fd;
    IFileSystem *fs;
    BaseFileAdaptor(int _fd, IFileSystem *_fs) : fd(_fd), fs(_fs) {
    }
    virtual ~BaseFileAdaptor() {
        close();
    }
    virtual int close() override final {
        if (fd < 0)
            return 0;
        int ret = UISysCall(::close(fd));
        if (ret == 0)
            fd = -1;
        return ret;
    }
    virtual IFileSystem *filesystem() override final {
        return fs;
    }
    virtual int fchmod(mode_t mode) override final {
        return UISysCall(::fchmod(fd, mode));
    }
    virtual int fchown(uid_t owner, gid_t group) override final {
        return UISysCall(::fchown(fd, owner, group));
    }
    virtual int fstat(struct stat *buf) override final {
        return UISysCall(::fstat(fd, buf));
    }
    virtual int ftruncate(off_t length) override final {
        return UISysCall(::ftruncate(fd, length));
    }
    virtual int sync_file_range(off_t offset, off_t nbytes, unsigned int flags) override {
        return fdatasync();
    }
#ifdef __linux__
    virtual int fiemap(struct fiemap *map) override {
        return UISysCall(::ioctl(fd, FS_IOC_FIEMAP, (::fiemap *)map));
    }
    virtual int fallocate(int mode, off_t offset, off_t len) override final {
        return UISysCall(::fallocate(fd, mode, offset, len));
    }
    virtual ssize_t fgetxattr(const char *name, void *value, size_t size) override final {
        return UISysCall(::fgetxattr(fd, name, value, size));
    }
    virtual ssize_t flistxattr(char *list, size_t size) override final {
        return UISysCall(::flistxattr(fd, list, size));
    }
    virtual int fsetxattr(const char *name, const void *value, size_t size,
                          int flags) override final {
        return UISysCall(::fsetxattr(fd, name, value, size, flags));
    }
    virtual int fremovexattr(const char *name) override final {
        return UISysCall(::fremovexattr(fd, name));
    }
#else
    UNIMPLEMENTED(ssize_t fgetxattr(const char *name, void *value, size_t size) override);
    UNIMPLEMENTED(ssize_t flistxattr(char *list, size_t size) override);
    UNIMPLEMENTED(int fsetxattr(const char *name, const void *value, size_t size, int flags)
                      override);
    UNIMPLEMENTED(int fremovexattr(const char *name) override);
#endif
};

class LocalFileAdaptor final : public BaseFileAdaptor {
public:
    using BaseFileAdaptor::BaseFileAdaptor;
    virtual off_t lseek(off_t offset, int whence) override {
        return UISysCall(::lseek(fd, offset, whence));
    }

    virtual int fsync() override {
        thread_yield();
        return UISysCall(::fsync(fd));
    }
    virtual ssize_t read(void *buf, size_t count) override {
        thread_yield();
        return UISysCall(::read(fd, buf, count));
    }
    virtual ssize_t readv(const struct iovec *iov, int iovcnt) override {
        thread_yield();
        return UISysCall(::readv(fd, iov, iovcnt));
    }
    virtual ssize_t write(const void *buf, size_t count) override {
        thread_yield();
        return UISysCall(::write(fd, buf, count));
    }
    virtual ssize_t writev(const struct iovec *iov, int iovcnt) override {
        thread_yield();
        return UISysCall(::writev(fd, iov, iovcnt));
    }
    virtual ssize_t pread(void *buf, size_t count, off_t offset) override {
        thread_yield();
        return UISysCall(::pread(fd, buf, count, offset));
    }
    virtual ssize_t pwrite(const void *buf, size_t count, off_t offset) override {
        thread_yield();
        return UISysCall(::pwrite(fd, buf, count, offset));
    }
#ifdef _GNU_SOURCE
    virtual int sync_file_range(off_t offset, off_t nbytes, unsigned int flags) override {
        thread_yield();
        return UISysCall(::sync_file_range(fd, offset, nbytes, flags));
    }
#endif
#ifdef __APPLE__
    UNIMPLEMENTED(ssize_t preadv(const struct iovec *iov, int iovcnt, off_t offset) override);
    UNIMPLEMENTED(ssize_t pwritev(const struct iovec *iov, int iovcnt, off_t offset) override);
    virtual int fdatasync() override {
        thread_yield();
        return UISysCall(::fcntl(fd, F_FULLFSYNC));
    }
#else
    virtual ssize_t preadv(const struct iovec *iov, int iovcnt, off_t offset) override {
        thread_yield();
        return UISysCall(::preadv(fd, iov, iovcnt, offset));
    }
    virtual ssize_t pwritev(const struct iovec *iov, int iovcnt, off_t offset) override {
        thread_yield();
        return UISysCall(::pwritev(fd, iov, iovcnt, offset));
    }
    virtual int fdatasync() override {
        thread_yield();
        return UISysCall(::fdatasync(fd));
    }
#endif
};

template <typename AIOEngine> class AioFileAdaptor final : public BaseFileAdaptor {
public:
    using BaseFileAdaptor::BaseFileAdaptor;
    virtual ssize_t pread(void *buf, size_t count, off_t offset) override {
        return AIOEngine::pread(fd, buf, count, offset);
    }
    virtual ssize_t preadv(const struct iovec *iov, int iovcnt, off_t offset) override {
        return AIOEngine::preadv(fd, iov, iovcnt, offset);
    }
    virtual ssize_t pwrite(const void *buf, size_t count, off_t offset) override {
        return AIOEngine::pwrite(fd, buf, count, offset);
    }
    virtual ssize_t pwritev(const struct iovec *iov, int iovcnt, off_t offset) override {
        return AIOEngine::pwritev(fd, iov, iovcnt, offset);
    }
    virtual int fsync() override {
        return AIOEngine::fsync(fd);
    }
    virtual int fdatasync() override {
        return AIOEngine::fdatasync(fd);
    }
};

class LocalDIR : public DIR {
public:
    ::DIR *dirp;
    ::dirent *direntp;
    long loc;
    LocalDIR(::DIR *dirp) : dirp(dirp) {
        next();
    }
    virtual ~LocalDIR() override {
        closedir();
    }
    virtual int closedir() override {
        if (dirp) {
            if (UISysCall(::closedir(dirp) == 0))
                dirp = nullptr;
        }

        return 0;
    }
    virtual dirent *get() override {
        return direntp;
    }
    virtual int next() override {
        if (dirp) {
            loc = UISysCall(::telldir(dirp));
            direntp = UISysCall(::readdir(dirp));
        }
        return direntp != nullptr ? 1 : 0;
    }
    virtual void rewinddir() override {
        ::rewinddir(dirp);
        next();
    }
    virtual void seekdir(long loc) override {
        ::seekdir(dirp, loc);
        next();
    }
    virtual long telldir() override {
        return loc;
    }
};

class LocalFileSystemAdaptor : public IFileSystem, public IFileSystemXAttr {
public:
    int io_engine_type = 0;
    int io_flags = 0;
    LocalFileSystemAdaptor(int ioengine_type) : io_engine_type(ioengine_type) {
#ifdef __linux__
        if (ioengine_type == ioengine_libaio) {
            LOG_INFO("using libaio, set io_flags O_DIRECT");
            this->io_flags = O_DIRECT;
        }
#endif
    }
    IFile *new_local_file(int fd) {
        if (fd < 0)
            LOG_ERRNO_RETURN(0, nullptr, "failed to create LocalFile object ");

        switch (io_engine_type) {
#ifdef __linux__
        case ioengine_posixaio:
            return new AioFileAdaptor<posixaio>(fd, this);
            break;
        case ioengine_libaio:
            return new AioFileAdaptor<libaio>(fd, this);
            break;
#endif
        default:
            return new LocalFileAdaptor(fd, this);
        }
    }

    virtual IFile *open(const char *pathname, int flags) override {
        int fd = UISysCall(::open(pathname, flags | io_flags));
        if (fd == -1)
            LOG_ERRNO_RETURN(0, nullptr, "failed to open file ", pathname);
        return new_local_file(fd);
    }
    virtual IFile *open(const char *pathname, int flags, mode_t mode) override {
        int fd = UISysCall(::open(pathname, flags | io_flags, mode));
        if (fd == -1)
            LOG_ERRNO_RETURN(0, nullptr, "failed to open file ", pathname);
        return new_local_file(fd);
    }
    virtual IFile *creat(const char *pathname, mode_t mode) override {
        int fd = UISysCall(::creat(pathname, mode));
        if (fd == -1)
            LOG_ERRNO_RETURN(0, nullptr, "failed to create file ", pathname);
        return new_local_file(fd);
    }
    virtual int mkdir(const char *pathname, mode_t mode) override {
        return UISysCall(::mkdir(pathname, mode));
    }
    virtual int rmdir(const char *pathname) override {
        return UISysCall(::rmdir(pathname));
    }
    virtual int symlink(const char *oldname, const char *newname) override {
        return UISysCall(::symlink(oldname, newname));
    }
    virtual ssize_t readlink(const char *pathname, char *buf, size_t bufsiz) override {
        return UISysCall(::readlink(pathname, buf, bufsiz));
    }
    virtual int link(const char *oldname, const char *newname) override {
        return UISysCall(::link(oldname, newname));
    }
    virtual int rename(const char *oldname, const char *newname) override {
        return UISysCall(::rename(oldname, newname));
    }
    virtual int unlink(const char *pathname) override {
        return UISysCall(::unlink(pathname));
    }
    virtual int chmod(const char *pathname, mode_t mode) override {
        return UISysCall(::chmod(pathname, mode));
    }
    virtual int chown(const char *pathname, uid_t owner, gid_t group) override {
        return UISysCall(::chown(pathname, owner, group));
    }
    virtual int lchown(const char *pathname, uid_t owner, gid_t group) override {
        return UISysCall(::lchown(pathname, owner, group));
    }
    virtual DIR *opendir(const char *pathname) override {
        ::DIR *dirp = UISysCall(::opendir(pathname));
        return dirp ? new LocalDIR(dirp) : nullptr;
    }
    virtual int stat(const char *path, struct stat *buf) override {
        return UISysCall(::stat(path, buf));
    }
    virtual int lstat(const char *path, struct stat *buf) override {
        return UISysCall(::lstat(path, buf));
    }
    virtual int access(const char *path, int mode) override {
        return UISysCall(::access(path, mode));
    }
    virtual int truncate(const char *path, off_t length) override {
        return UISysCall(::truncate(path, length));
    }
    virtual int syncfs() override {
        ::sync();
        return 0;
    }
#ifdef __linux__
    virtual int statfs(const char *path, struct statfs *buf) override {
        return UISysCall(::statfs(path, buf));
    }
    virtual int statvfs(const char *path, struct statvfs *buf) override {
        return UISysCall(::statvfs(path, buf));
    }
    virtual ssize_t getxattr(const char *path, const char *name, void *value,
                             size_t size) override {
        return UISysCall(::getxattr(path, name, value, size));
    }
    virtual ssize_t lgetxattr(const char *path, const char *name, void *value,
                              size_t size) override {
        return UISysCall(::lgetxattr(path, name, value, size));
    }
    virtual ssize_t listxattr(const char *path, char *list, size_t size) override {
        return UISysCall(::listxattr(path, list, size));
    }
    virtual ssize_t llistxattr(const char *path, char *list, size_t size) override {
        return UISysCall(::llistxattr(path, list, size));
    }
    virtual int setxattr(const char *path, const char *name, const void *value, size_t size,
                         int flags) override {
        return UISysCall(::setxattr(path, name, value, size, flags));
    }
    virtual int lsetxattr(const char *path, const char *name, const void *value, size_t size,
                          int flags) override {
        return UISysCall(::lsetxattr(path, name, value, size, flags));
    }
    virtual int removexattr(const char *path, const char *name) override {
        return UISysCall(::removexattr(path, name));
    }
    virtual int lremovexattr(const char *path, const char *name) override {
        return UISysCall(::lremovexattr(path, name));
    }
#else
    UNIMPLEMENTED(int statfs(const char *path, struct statfs *buf) override);
    UNIMPLEMENTED(int statvfs(const char *path, struct statvfs *buf) override);
    UNIMPLEMENTED(ssize_t getxattr(const char *path, const char *name, void *value, size_t size)
                      override);
    UNIMPLEMENTED(ssize_t lgetxattr(const char *path, const char *name, void *value, size_t size)
                      override);
    UNIMPLEMENTED(ssize_t listxattr(const char *path, char *list, size_t size) override);
    UNIMPLEMENTED(ssize_t llistxattr(const char *path, char *list, size_t size) override);
    UNIMPLEMENTED(int setxattr(const char *path, const char *name, const void *value, size_t size,
                               int flags) override);
    UNIMPLEMENTED(int lsetxattr(const char *path, const char *name, const void *value, size_t size,
                                int flags) override);
    UNIMPLEMENTED(int removexattr(const char *path, const char *name) override);
    UNIMPLEMENTED(int lremovexattr(const char *path, const char *name) override);
#endif
};

IFileSystem *new_localfs_adaptor(const char *root_path, int io_engine_type) {
    auto lfs = new LocalFileSystemAdaptor(io_engine_type);
    if (!root_path || !root_path[0])
        return lfs;

    auto sfs = new_subfs(lfs, root_path, true);
    if (!sfs) {
        delete lfs;
        return nullptr;
    }
    return sfs;
}

IFile *new_localfile_adaptor(int fd, int io_engine_type) {
    if (fd < 0)
        LOG_ERROR_RETURN(EINVAL, nullptr, "invalid fd: ", fd);

#ifdef __linux__
    if (io_engine_type == ioengine_libaio)
        return new AioFileAdaptor<libaio>(fd, nullptr);

    if (io_engine_type == ioengine_posixaio)
        return new AioFileAdaptor<posixaio>(fd, nullptr);
#endif

    return new LocalFileAdaptor(fd, nullptr);
}

IFile *open_localfile_adaptor(const char *filename, int flags, mode_t mode, int io_engine_type) {
    int fd = UISysCall(::open(filename, flags, mode));
    if (fd < 0)
        LOG_ERRNO_RETURN(0, nullptr, "failed to ::open('`', `, `)", filename, flags, mode);

    return new_localfile_adaptor(fd, io_engine_type);
}
} // namespace FileSystem
