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

void main() {
#if <n> >= 1
#if <0_is_integer>
    fs_out0 = <vec_type0>(2);
#else
    fs_out0 = vec4(0.6);
#endif
#endif

#if <n> >= 2
#if <1_is_integer>
    fs_out1 = <vec_type1>(3);
#else
    fs_out1 = vec4(0.65);
#endif
#endif

#if <n> >= 3
#if <2_is_integer>
    fs_out2 = <vec_type2>(4);
#else
    fs_out2 = vec4(0.7);
#endif
#endif

#if <n> >= 4
#if <3_is_integer>
    fs_out3 = <vec_type3>(5);
#else
    fs_out3 = vec4(0.75);
#endif
#endif
}
