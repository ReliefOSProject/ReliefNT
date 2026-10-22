======================
描述符、块设备与时间戳
======================

授权与身份
==========

打开时执行 DAC，读写检查描述符访问模式，O_PATH 不能进行普通读写。
重读分区表 BLKRRPART 要求 CAP_SYS_ADMIN，不依赖旧桌面角色。
utimensat(fd, NULL, times, 0) 更新该 fd 持有的 inode，改名或 unlink 不把它
重定向到另一个同名文件；O_PATH 应拒绝。

Linux 请求值
============

BLKROGET 使用 Linux 历史 _IO(0x12,94) 编码，旧错误 _IOR 编码不作为新契约。
使用旧头固化请求值的应用需以当前 UAPI 重编译。内部 timerfd/eventfd 状态
不能与 O_SYNC/O_TMPFILE 位重叠；同步操作要实际完成刷新并传播失败。
独占块打开应正确检查真实挂载冲突，不把匿名 RAM/光盘挂载误认为其他设备占用。
