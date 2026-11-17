/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.process

import std.io.*

// JDK io_util.c readBytes/writeBytes: the blocking read/write runs on a native
// buffer and only the bounded copy holds the array raw data. A thread blocked in
// I/O inside acquireArrayRawData would hold a ZJNICritical region, so a GC pause
// would wait on it and every later acquirer would wait on the pause.
foreign func memcpy_s(dest: CPointer<UInt8>, destMax: UIntNative, src: CPointer<UInt8>, count: UIntNative): Int32

private let NATIVE_COPY_MAX_LEN: Int64 = Int64(Int32.Max)

private unsafe func copyNativeBytes(dest: CPointer<Byte>, src: CPointer<Byte>, count: Int64): Int32 {
    var offset: Int64 = 0
    while (offset < count) {
        let remain = count - offset
        let n = if (remain < NATIVE_COPY_MAX_LEN) { remain } else { NATIVE_COPY_MAX_LEN }
        let rc = memcpy_s(dest + offset, UIntNative(n), src + offset, UIntNative(n))
        if (rc != 0) {
            return rc
        }
        offset += n
    }
    return 0
}

/**
 * ProcessOutputStream implement OutputStream & Resource interface
 *
 */
const SEEK_CUR: Int32 = 1 /* Seek from current position.  */

struct FileDescriptor {
    var _fileHandle: IntNative = INVALID_HANDLE

    init(fileHandle: IntNative) {
        this._fileHandle = fileHandle
    }
}

class ProcessInputStream <: InputStream {
    var inputStream: InputStream

    init(fileDescriptor: FileDescriptor) {
        inputStream = PipeInputStream(fileDescriptor)
    }

    public func read(buffer: Array<Byte>): Int64 {
        this.inputStream.read(buffer)
    }
}

class ProcessOutputStream <: OutputStream {
    var outputStream: OutputStream

    init(fileDescriptor: FileDescriptor) {
        outputStream = PipeOutputStream(fileDescriptor)
    }

    public func write(buffer: Array<Byte>): Unit {
        this.outputStream.write(buffer)
    }

    public func flush(): Unit {}
}

/**
 * PipeOutputStream implement OutputStream & Resource interface
 */
class PipeOutputStream <: OutputStream & Resource {
    var _fileDescriptor: FileDescriptor = FileDescriptor(INVALID_HANDLE)

    init(fileDescriptor: FileDescriptor) {
        this._fileDescriptor = fileDescriptor
    }

    public func write(buffer: Array<Byte>): Unit {
        if (!isHandleValid(_fileDescriptor._fileHandle)) {
            throw ProcessException("Stream is not opened.")
        }
        if (buffer.size == 0) {
            return
        }
        this.directWrite(buffer)
    }

    public func flush(): Unit {}

    public func isClosed(): Bool {
        return !isHandleValid(_fileDescriptor._fileHandle)
    }

    public func close(): Unit {
        let handle = _fileDescriptor._fileHandle
        if (!isHandleValid(handle)) {
            return
        }
        this.flush()
        closePipeHandle(handle)
        this._fileDescriptor._fileHandle = INVALID_HANDLE
    }

    /**
     * Write the array to the file and return the bytes successfully written into the file.
     *
     * @param buffer – The buffer will be written to the file
     *
     *
     * @throws ProcessException if system failed to write the file
     */
    private func directWrite(buffer: Array<Byte>): Unit {
        unsafe {
            let bufSize: Int64 = buffer.size
            let native: CPointer<Byte> = LibC.malloc<Byte>(count: bufSize)
            if (native.isNull()) {
                throw ProcessException("Stream I/O error: failed to allocate a native buffer of ${bufSize} bytes.")
            }
            let bufPtr: CPointerHandle<Byte> = acquireArrayRawData(buffer)
            let rc = copyNativeBytes(native, bufPtr.pointer, bufSize)
            releaseArrayRawData(bufPtr)
            if (rc != 0) {
                LibC.free(native)
                throw ProcessException("Stream write error: copy failed with error code '${rc}'.")
            }
            let writeSuccess: Bool = CJ_OS_FileWrite(this._fileDescriptor._fileHandle, native, UIntNative(bufSize))
            LibC.free(native)
            if (!writeSuccess) {
                throw ProcessException("Stream write error.")
            }
        }
    }
}

/**
 * PipeInputStream implement InputStream & Resource interface
 */
class PipeInputStream <: InputStream & Resource {
    var _fileDescriptor: FileDescriptor = FileDescriptor(INVALID_HANDLE)

    init(fileDescriptor: FileDescriptor) {
        this._fileDescriptor = fileDescriptor
    }

    public func read(buffer: Array<Byte>): Int64 {
        if (!isHandleValid(_fileDescriptor._fileHandle)) {
            throw ProcessException("Stream is not opened.")
        }

        if (buffer.size == 0) {
            throw IllegalArgumentException("Buffer is empty.")
        }

        let readBytes: Int64 = directRead(buffer)
        if (readBytes < 0) {
            throw ProcessException("Stream read error.")
        }
        return readBytes
    }

    public func isClosed(): Bool {
        return !isHandleValid(_fileDescriptor._fileHandle)
    }

    public func close(): Unit {
        let handle = _fileDescriptor._fileHandle
        if (!isHandleValid(handle)) {
            return
        }
        closePipeHandle(handle)
        this._fileDescriptor._fileHandle = INVALID_HANDLE
    }

    /**
     * Read from the file directly and write into buffer.
     *
     * @param buffer - Read to the buffer from the file
     *
     * @return Length successfully written into the array buffer
     */
    private func directRead(buffer: Array<Byte>): Int64 {
        unsafe {
            let bufSize: Int64 = buffer.size
            let native: CPointer<Byte> = LibC.malloc<Byte>(count: bufSize)
            if (native.isNull()) {
                throw ProcessException("Stream I/O error: failed to allocate a native buffer of ${bufSize} bytes.")
            }
            let readSize: Int64 = CJ_OS_FileRead(this._fileDescriptor._fileHandle, native, UIntNative(bufSize))
            var rc: Int32 = 0
            if (readSize > 0) {
                let bufPtr: CPointerHandle<Byte> = acquireArrayRawData(buffer)
                rc = copyNativeBytes(bufPtr.pointer, native, readSize)
                releaseArrayRawData(bufPtr)
            }
            LibC.free(native)
            if (rc != 0) {
                throw ProcessException("Stream read error: copy failed with error code '${rc}'.")
            }
            return readSize
        }
    }
}

class NullProcessStream <: IOStream & Resource {
    var _fileDescriptor: FileDescriptor = FileDescriptor(INVALID_HANDLE)

    init() {}

    public func read(_: Array<Byte>): Int64 {
        return -1
    }

    public func write(_: Array<Byte>): Unit {
        throw ProcessException("Invalid output stream.")
    }

    public func flush(): Unit {}

    public func isClosed(): Bool {
        return !isHandleValid(_fileDescriptor._fileHandle)
    }

    public func close(): Unit {}
}

func closePipeHandle(handle: IntNative): Unit {
    if (unsafe { CJ_OS_CloseFile(handle) } < 0) {
        throw ProcessException("Failed to close stream.")
    }
}
