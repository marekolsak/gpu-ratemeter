#version 460

#if <n> >= 1
layout(location = 0) out <vec_type0> fs_out0;
#endif

#if <n> >= 2
layout(location = 1) out <vec_type1> fs_out1;
#endif

#if <n> >= 3
layout(location = 2) out <vec_type2> fs_out2;
#endif

#if <n> >= 4
layout(location = 3) out <vec_type3> fs_out3;
#endif

#if <n> >= 5
#error "n is too large"
#endif

layout(location = 0) in vec4 vColor;

void main() {
#if <n> >= 1
    fs_out0 = <vec_type0>(vColor * <multiplier0>);
#endif

#if <n> >= 2
    fs_out1 = <vec_type1>(vColor * <multiplier1>);
#endif

#if <n> >= 3
    fs_out2 = <vec_type2>(vColor * <multiplier2>);
#endif

#if <n> >= 4
    fs_out3 = <vec_type3>(vColor * <multiplier3>);
#endif
}
