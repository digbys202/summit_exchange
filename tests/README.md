# Summit Exchange Tests

These tests use synthetic MIDI and do not connect to a Summit or REAPER. They require a C++ compiler, CMake, Git, `pkg-config`, and FreeType/fontconfig development libraries.

Build the [ysfx](https://github.com/jpcima/ysfx) test library:

```bash
git clone --depth 1 https://github.com/jpcima/ysfx.git "$HOME/.cache/summit-exchange-tests/ysfx"
git -C "$HOME/.cache/summit-exchange-tests/ysfx" submodule update --init thirdparty/dr_libs
cmake -S "$HOME/.cache/summit-exchange-tests/ysfx" -B "$HOME/.cache/summit-exchange-tests/ysfx/build" -DYSFX_PLUGIN=OFF -DYSFX_GFX=ON -DYSFX_TOOLS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build "$HOME/.cache/summit-exchange-tests/ysfx/build" -j2
```

From the repository root, build and run the Summit suite:

```bash
c++ -std=c++11 -I "$HOME/.cache/summit-exchange-tests/ysfx/include" tests/summit_exchange_test.cpp "$HOME/.cache/summit-exchange-tests/ysfx/build/libysfx.a" $(pkg-config --libs freetype2 fontconfig) -lpthread -ldl -o "$HOME/.cache/summit-exchange-tests/summit_test"
"$HOME/.cache/summit-exchange-tests/summit_test" "$PWD/Effects/Summit Exchange/Summit Exchange.jsfx" "$PWD/tests/summit_sender_test_controller.jsfx"
```

The test renders the interface to `/tmp/summit-exchange.ppm`. Live hardware behavior must be verified separately.
