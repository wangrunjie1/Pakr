# 游戏GIF上传选择修正（2026-10-31）

MainActivity的onActivityResult原来先判断data或data.data为空，就返回预先生成的camera_时间.jpg；这会错误吞掉存在clipData的多选文件，导致网页读取到未拍照的相机占位文件。现在优先clipData、其次data.data，最后仅在相机URI确实可读且非空时返回相机图片。每次选择先清除旧相机URI，GIF-only输入不添加拍照入口。没有修改网页解码或转码流程。

状态：仅本地Android模板源码修复，未提交或推送此模板仓库，未发布新APK。当前游戏网页服务器发布不能改变已安装APK的Java/Kotlin代码，需要按原应用包名和签名重新打包并安装更新。

按上级AGENTS约定，本次未执行Android构建、打包或类型检查。需要真机验证：选择单GIF、多GIF（clipData无data）、取消文件选择、实际拍照、取消拍照，以及GIF-only选择器不提供拍照，确认上传原字节及动画保留。没有声称现有APK已经修好。
