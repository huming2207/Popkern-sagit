# Popkern for Sagit (Xiaomi Mi 6)

Yet another optimised OSS kernel for Xiaomi Mi 6, driven by `git cherry-pick` (at least for now lol)...

# How to compile 

1. Download the source: `git clone https://github.com/huming2207/Popkern-sagit.git`

2. Perpare toolchain: you can either choose android perbuilt kernel, or build by your own via [crosstool-ng](https://github.com/crosstool-ng/crosstool-ng).

3. Set environment variable: 

```bash
export CROSS_COMPILE=aarch64-your_toolchain-linux-android-

export ARCH=arm64
```

4. Start to build

```bash
make defconfig sagit_user_defconfig
make dtbs
make -j6
```

5. Merge DTBs to kernel binary

    Xiaomi's kernel uses kernel built-in DTBs, not the standalone `dt.img`. As a result, we just need to merge those single DTB files (under `arch/arm64/boot/dts/qcom`) to kernel binary and it should works.

    I know there is a `Image.gz-dtb` file under `arch/arm64/boot` but it is exactly the same as `Image.gz`. Those DTB files actually haven't been merged when compile. As a result, we have to do some DIYs instead.

    The command is:

    ```bash
    cd arch/arm64/boot
    echo Image.gz dts/qcom/*dtb > Image.gz-realdtb
    ```

6. Flash to your phone

    Download an existing Mi 6's AnyKernel2 flashable zip package, for example: [TODO](127.0.0.1)

    Then unzip the package, copy the `Image.gz-realdtb` to the unzipped file path, and replace the original kernel binary.

    When finishes, re-pack the files by `zip -r9 UPDATE-sagit-AnyKernel2.zip * -x README UPDATE-sagit-AnyKernel2.zip`


# Credit 

- Franco Kernel for OnePlus 5: [https://github.com/franciscofranco/cheeseburger](https://github.com/franciscofranco/cheeseburger)

- Blu spark kernel for OnePlus 5: [https://forum.xda-developers.com/oneplus-5/development/kernel-t3651933](https://forum.xda-developers.com/oneplus-5/development/kernel-t3651933)

- Alucard kernel for OnePlus 5: [https://github.com/Alucard24/Alucard-Kernel-cheeseburger](https://github.com/Alucard24/Alucard-Kernel-cheeseburger)

- Boeffla kernel for OnePlus 5: [https://github.com/andip71/boeffla-kernel-oos-oneplus5](https://github.com/andip71/boeffla-kernel-oos-oneplus5)