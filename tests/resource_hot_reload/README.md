# 资源热重载与重复卸载压力测试

该测试用真实的 raylib `Texture2D`、`Model`、`Sound` 资源和公开
`Load/Unload` API 模拟资源热重载。raylib 本身不提供资源引用计数，因此测试内
实现了一个带代际句柄和引用计数的最小资源层，用来验证生命周期契约。

## 覆盖范围

- 固定 200 轮，每轮依次处理纹理、模型、音频，共 600 个场景。
- 每个场景执行创建、两次额外 acquire、真实资源使用、一次热重载、三次
  release，以及释放后的重复 release/use/reload。
- 热重载交替使用两份真实资产；失败时旧资源保持不变，引用计数不变。
- 每 5 个场景注入一次真实缺文件失败；每 7 个场景注入一次“加载成功后失败”，
  用于验证半途失败时的完整清理。
- 每个场景混入越界索引、错误 magic、零 generation、错误资源类型、过期句柄和
  零句柄。
- 每个场景结束以及测试结束都检查 `live == 0`、`totalRefs == 0` 和槽位数据
  已清零。

## 运行

```bash
cmake -S . -B build-resource-test \
  -DBUILD_TESTS=ON \
  -DBUILD_EXAMPLES=OFF \
  -DENABLE_ASAN=ON \
  -DENABLE_UBSAN=ON
cmake --build build-resource-test -j
ctest --test-dir build-resource-test --output-on-failure
```

桌面环境需要可用的 OpenGL 上下文。无显示环境可使用
`xvfb-run -a ctest --test-dir build-resource-test --output-on-failure`。
Linux 上没有可用音频设备时，测试会自动使用同目录下的 `alsa-null.conf`，
仅将 ALSA 输出端点替换为空设备，资源解码和缓冲区生命周期仍走真实代码路径。

也可以单独运行：

```bash
./build-resource-test/tests/resource_hot_reload/resource_hot_reload
```

## 本次结果

2026-11-18 在本仓库执行 ASAN + UBSAN 构建，CTest 与单独运行均通过：

```text
RESULT: PASS
rounds: 200
scenarios: 600 hot_reloads=600 ok=411 injected_fail=86 actual_fail=103
acquires=1200 releases=1800 uses=600
invalid_ops=6000 stale=1800 wrong_kind=1200 oob=600 bad_magic=2400 dead=0
cleaned_partial_loads=189 peak_refs=3 peak_live=1
final_live=0 final_refs=0
```

ASAN 未发现内存泄漏，UBSAN 未发现未定义行为，进程退出码为 0。

`releases=1800` 包含每个场景的三次合法 release；释放后的重复 release 属于
`invalid_ops`，不会再次调用底层 unload。`cleaned_partial_loads=189` 与
`injected_fail + actual_fail` 相等，表示所有失败路径都完成了清理。

## 反例

测试会主动构造并验证以下反例，而不是只覆盖正常路径：

| 反例 | 构造方式 | 期望结果 |
| --- | --- | --- |
| 伪造句柄 | 翻转 magic 字节 | 拒绝，不访问资源槽 |
| 越界索引 | `index = SLOT_COUNT + 3` | 拒绝 acquire/use |
| 零 generation | generation 编码为 0 | 拒绝 acquire |
| 错误类型 | 用纹理句柄访问模型槽 | 拒绝 acquire/reload |
| 释放后重复卸载 | release 成功后再 release 同一句柄 | 拒绝，不重复 unload |
| 释放后使用/重载 | 对过期句柄执行 use/reload | 拒绝，引用计数保持 0 |
| 缺文件加载 | 使用不存在的 `.png/.obj/.wav` | 返回失败，已分配的局部数据被清理 |
| 加载后失败 | 成功加载后强制走失败分支 | 新资源立即 unload，旧资源及 refcount 不变 |

`Sound` 失败路径同时覆盖解码失败，以及音频缓冲已创建后的回滚清理。

## 失败序列输出

每个资源槽维护一份独立的操作日志。任一不变量失败时，测试只打印该句柄自最近
干净场景开始的操作，而不是整个 600 场景日志；无法关联到具体句柄的全局失败则
回退到当前场景轨迹。

示例：

```text
FAIL: resource_hot_reload.c:720 (round 17)
reload changed refcount: 1 != 3

minimal operation sequence:
  handle=texture[0] generation=1 refs=1
  #2 CREATE texture[0] ... refs=1 generation=1
  #10 ACQUIRE refs=2
  #11 ACQUIRE refs=3
  #12 USE ok refs=3
  #13 RELOAD begin old=... new=... refs=3
  #13 RELOAD failed -> old resource retained refs=3
state: live=1 totalRefs=3
```

这段序列可以直接按顺序重放，并定位到第一个破坏引用计数的操作。
