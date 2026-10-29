# cpp-pi

pi（earendil-works/pi）的 C++ 重写：**前后端分离 · DDD 四层 · DCI 三层组合 · trans-dsl 事务化编排**。

- 前端：复用 pi 的 TS TUI，经 NDJSON RPC 协议接入（协议见 `rpc-types.ts`）
- 后端：本仓库，C++17

设计与迁移路线见 [DESIGN.md](DESIGN.md)。

## 构建

```bash
# trans-dsl-2 需与本仓库同级(或 -DTDSL_ROOT=<path>)
cmake -B build && cmake --build build
./build/cpp-pi            # CLI REPL (骨架演示)
./build/cpp-pi --mode rpc # RPC 后端 (待实现)
```

## 结构

```
src/pi/
├── domain/          值对象 + Session 聚合 + 端口(ILlmProvider/IToolExecutor/ISessionStore)
├── application/
│   ├── context/     context 碎片(EventQueueCtx/TurnStateCtx/SessionCtx) + context 树
│   ├── roles/       业务 role(Completer/ToolRunner/Persister), USE_ROLE 自感知
│   ├── actions/     trans-dsl 薄壳 action
│   ├── AgentTurnService.h   role 树
│   ├── AgentTurnImpl.h      service + context 合流, IMPL_ROLE 补槽位
│   └── AgentTurn.h          __transaction + 监听器
├── infrastructure/  端口实现(Mock provider / 本地工具 / 内存 store; 正式版走 folly+proxygen)
└── presentation/    cli (REPL 演示), rpc (NDJSON, 待实现)
```
