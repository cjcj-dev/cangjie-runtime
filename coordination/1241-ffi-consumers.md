待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。

# 修改前消费者核验

坐标 0df6f87b6bd6fecca643a5c0c4fa05d6131047d0。

命令：`rg -n "acquire.rawdata|acquireRawData|acquireArrayRawData|\.acquireRaw\(" stdlib/libs /root/cj_build/cjcj/packages/codegen`。

直接 intrinsic 消费者：

|file:line|零长解引用结论|
|---|---|
|array_intrinsic.cj:15|声明；无解引用|
|array_common.cj:450|指针偏移并包装 CPointerHandle；无解引用；release :455 还原偏移|
|string.cj:106|cSize==0 在 :94 返回；此分支 cSize>=阈值|
|string.cj:136|偏移返回，无解引用；:142 release 还原偏移|
|string.cj:840,841,873,874|小于阈值走有界循环，原生 memcmp 分支长度非零|
|string.cj:1313,1314|indexOfString 按 strSize/size/startIndex 传 native，native/string.c:129 检查范围；单字节模式只读非空 pattern|
|string.cj:1341,1342|native/string.c:211 在 orgSize<=0 或 subSize<=0 返回，无解引用|
|string.cj:1534|length<阈值在 :1523 有界循环；此分支非零|

包装消费者：

|分组与锚|零长路径|
|---|---|
|string.cj:476,477,586,587|CountString :173 只在 subSize==1 读 pattern[0]；orgSize<=0 :176 返回；CountAndIndexString :211 判两长度|
|libc.cj:64|:61 length==0 先返回|
|core/print.cj:14,33,47,61|native/print.c:25 fwrite(...,len)，Windows :43 while remainingLen>0|
|core/thread.cj:321|:325 传 newName.size|
|time/zone_info_read.cj:55,65,66; timezone.cj:297,307|native/time_common.c:129 GetCPath 在 pathLen<=0 返回；读取 buffer=fileSize+1|
|process/process.cj:786,787|:805 ProcessStartInfo 显式传 cmdArg/environment 数量；None 环境另传 null|
|fs/file.cj:338; directory.cj:68; path.cj:336|临时名追加后缀及终止符；realPathArr=PATH_MAX_SIZE，非零|
|fs/file.cj:540,559; process/process_stream.cj:100,159|read/write 显式传 buffer.size|
|console/console_writer.cj:198,214,230; env/console_writer.cj:198,214,232|固定 bufferSize=512；无零长底层数组|
|console/console_writer.cj:256; env/console_writer.cj:260|CJ_CONSOLE_Write 传剩余计数|
|console/console_reader.cj:191; env/console_reader.cj:177|reserve 后 memcpy_s 传 itemLen|
|io/string_writer.cj:94,132,159|ensureEnoughOutBuf 在 acquire 前，memcpy/格式化传容量|
|core/c_string_resource.cj:207; unittest/common/json_write_buffer.cj:119|memcpy_s 传 remain/cpSize；目标提前扩容|
|net/socket_raw_cjnative.cj:396,425,451,471,603,638,699,718; socket_ffi_cjnative_impl.cj:393,449,540,580,626,642|send/recv/copy 全部传当前长度，不比较哨兵|
|net/socket_ffi.cj:277|native/utils.c:84 分配 size 并 memcpy_s(...,size)，无零长无条件解引用|
|core/string_common.cj:385,386|native/string.c:211 的 CountAndIndexString 两长度守卫|

没有在本次枚举中发现依赖不可读页哨兵的消费者。此结论范围是工作树 std 与编译器 intrinsic 分派，不声称覆盖外部用户 FFI。

## 原始包装消费者检索输出
```
stdlib/libs/std/console/console_reader.cj:191:            let itemP: CPointerHandle<UInt8> = acquireArrayRawData(this.buffer)
stdlib/libs/std/console/console_writer.cj:198:                let cp = acquireArrayRawData(this.buffer_)
stdlib/libs/std/console/console_writer.cj:214:                let cp = acquireArrayRawData(this.buffer_)
stdlib/libs/std/console/console_writer.cj:230:                let cp = acquireArrayRawData(this.buffer_)
stdlib/libs/std/console/console_writer.cj:256:            var ptr = acquireArrayRawData(arr)
stdlib/libs/std/io/string_writer.cj:94:            let cp = acquireArrayRawData(outputBOS.outBuf)
stdlib/libs/std/io/string_writer.cj:132:            let cp = acquireArrayRawData(outputBOS.outBuf)
stdlib/libs/std/io/string_writer.cj:159:            let cp = acquireArrayRawData(this.outputBOS.outBuf)
stdlib/libs/std/fs/file.cj:338:            var arrPtr: CPointerHandle<Byte> = acquireArrayRawData(tempArr)
stdlib/libs/std/fs/file.cj:540:            let bufPtr: CPointerHandle<Byte> = acquireArrayRawData(buffer)
stdlib/libs/std/fs/file.cj:559:            var bufPtr: CPointerHandle<Byte> = acquireArrayRawData(buffer)
stdlib/libs/std/core/string.cj:476:            var ptr1: CPointer<UInt8> = this.acquireRaw()
stdlib/libs/std/core/string.cj:477:            var ptr2: CPointer<UInt8> = str.acquireRaw()
stdlib/libs/std/core/string.cj:586:            let org: CPointer<UInt8> = this.acquireRaw()
stdlib/libs/std/core/string.cj:587:            let pat: CPointer<UInt8> = old.acquireRaw()
stdlib/libs/std/process/process_stream.cj:100:            var bufPtr: CPointerHandle<Byte> = acquireArrayRawData(buffer)
stdlib/libs/std/process/process_stream.cj:159:            let bufPtr: CPointerHandle<Byte> = acquireArrayRawData(buffer)
stdlib/libs/std/core/libc.cj:64:        let ptrArr: CPointer<UInt8> = str.acquireRaw()
stdlib/libs/std/fs/path.cj:336:            var arrPtr: CPointerHandle<Byte> = acquireArrayRawData(realPathArr)
stdlib/libs/std/core/array_common.cj:429:    @Deprecated[message: "Use global function `public unsafe func acquireArrayRawData<T>(arr: Array<T>): CPointerHandle<T> where T <: CType` instead."]
stdlib/libs/std/core/array_common.cj:435:    @Deprecated[message: "Use global function `public unsafe func acquireArrayRawData<T>(arr: Array<T>): CPointerHandle<T> where T <: CType` instead."]
stdlib/libs/std/core/array_common.cj:444: * trigger garbage collection between func acquireArrayRawData<T>(arr: Array<T>)
stdlib/libs/std/core/array_common.cj:449:public unsafe func acquireArrayRawData<T>(arr: Array<T>): CPointerHandle<T> where T <: CType {
stdlib/libs/std/net/socket_raw_cjnative.cj:396:                let bufCp = acquireArrayRawData(buffer)
stdlib/libs/std/net/socket_raw_cjnative.cj:425:                let bufCp = acquireArrayRawData(buffer)
stdlib/libs/std/net/socket_raw_cjnative.cj:451:                let bufCp = acquireArrayRawData(buffer)
stdlib/libs/std/net/socket_raw_cjnative.cj:471:                let bufCp = acquireArrayRawData(buffer)
stdlib/libs/std/net/socket_raw_cjnative.cj:603:            let bufCp = acquireArrayRawData(buffer)
stdlib/libs/std/net/socket_raw_cjnative.cj:638:                let bufCp = acquireArrayRawData(buffer)
stdlib/libs/std/net/socket_raw_cjnative.cj:699:            let bufCp = acquireArrayRawData(buffer)
stdlib/libs/std/net/socket_raw_cjnative.cj:718:                let bufCp = acquireArrayRawData(buffer)
stdlib/libs/std/fs/directory.cj:68:            var arrPtr: CPointerHandle<Byte> = acquireArrayRawData(tempArr)
stdlib/libs/std/process/process.cj:786:    let cmdArg_cpHandle: CPointerHandle<CString> = unsafe { acquireArrayRawData<CString>(cmdArg_cArr) }
stdlib/libs/std/process/process.cj:787:    let environment_cpHandle: CPointerHandle<CString> = unsafe { acquireArrayRawData<CString>(environment_cArr) }
stdlib/libs/std/net/socket_ffi_cjnative_impl.cj:393:            let bufCp = acquireArrayRawData(buffer)
stdlib/libs/std/net/socket_ffi_cjnative_impl.cj:449:            let bufCp = acquireArrayRawData(buffer)
stdlib/libs/std/net/socket_ffi_cjnative_impl.cj:540:                let bufCp = acquireArrayRawData(buffer)
stdlib/libs/std/net/socket_ffi_cjnative_impl.cj:580:                let bufCp = acquireArrayRawData(buffer)
stdlib/libs/std/net/socket_ffi_cjnative_impl.cj:626:            let bufCp = acquireArrayRawData(buffer)
stdlib/libs/std/net/socket_ffi_cjnative_impl.cj:642:            let bufCp = acquireArrayRawData(buffer)
stdlib/libs/std/core/c_string_resource.cj:207:                let rawData: CPointerHandle<UInt8> = acquireArrayRawData(data)
stdlib/libs/std/net/socket_ffi.cj:277:        let handle = acquireArrayRawData(addr)
stdlib/libs/std/core/thread.cj:321:            let handle = unsafe { acquireArrayRawData(newName.rawData()) }
stdlib/libs/std/core/print.cj:14:        var cp = acquireArrayRawData(str.rawData())
stdlib/libs/std/core/print.cj:33:        var cp = acquireArrayRawData(str.rawData())
stdlib/libs/std/core/print.cj:47:        var cp = acquireArrayRawData(str.rawData())
stdlib/libs/std/core/print.cj:61:        var cp = acquireArrayRawData(str.rawData())
stdlib/libs/std/env/console_writer.cj:198:                let cp = acquireArrayRawData(this.buffer)
stdlib/libs/std/env/console_writer.cj:214:                let cp = acquireArrayRawData(this.buffer)
stdlib/libs/std/env/console_writer.cj:232:                let cp = acquireArrayRawData(this.buffer)
stdlib/libs/std/env/console_writer.cj:260:            var ptr = acquireArrayRawData(arr)
stdlib/libs/std/core/string_common.cj:385:                let org: CPointerHandle<UInt8> = acquireArrayRawData<UInt8>(this.orgData)
stdlib/libs/std/core/string_common.cj:386:                let pat: CPointerHandle<UInt8> = acquireArrayRawData<UInt8>(str.rawData())
stdlib/libs/std/time/zone_info_read.cj:55:        let cPath: CPointerHandle<UInt8> = acquireArrayRawData(path.rawData())
stdlib/libs/std/time/zone_info_read.cj:65:        let cPath2: CPointerHandle<UInt8> = acquireArrayRawData(path.rawData())
stdlib/libs/std/time/zone_info_read.cj:66:        var arrPtr: CPointerHandle<UInt8> = acquireArrayRawData(arr)
stdlib/libs/std/env/console_reader.cj:177:        let data: CPointerHandle<UInt8> = acquireArrayRawData(this.buffer)
stdlib/libs/std/time/timezone.cj:297:                let cPath: CPointerHandle<UInt8> = acquireArrayRawData(absPath.rawData())
stdlib/libs/std/time/timezone.cj:307:            let cPath: CPointerHandle<UInt8> = acquireArrayRawData(ANDROID_TZDATA.rawData())
stdlib/libs/std/unittest/common/json_write_buffer.cj:119:            let dest = acquireArrayRawData(_buffer)

```