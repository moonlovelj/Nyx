import numpy as np
import argparse
import matplotlib.pyplot as plt
import sys

# --------------------------------------------------------------------------
# 导入 Bridson_sampling 函数
# --------------------------------------------------------------------------
try:
    from poisson_disc import Bridson_sampling
except ImportError:
    print("[错误] 无法导入 'Bridson_sampling' 函数，请确保 poisson_disc 库已安装。")
    sys.exit()

# --------------------------------------------------------------------------
def format_for_shader(samples, lang="hlsl"):
    num_samples = samples.shape[0]
    if num_samples == 0:
        return f"// No samples generated for {lang.upper()}"
    
    if lang.lower() == "hlsl":
        output = f"static const float2 POISSON_SAMPLES[{num_samples}] = {{\n"
        template = "    float2({x:.6f}, {y:.6f})"
        output += ",\n".join([template.format(x=s[0], y=s[1]) for s in samples])
        output += "\n};"
    else:  # glsl
        output = f"const vec2 POISSON_SAMPLES[{num_samples}] = vec2[](\n"
        template = "    vec2({x:.6f}, {y:.6f})"
        output += ",\n".join([template.format(x=s[0], y=s[1]) for s in samples])
        output += "\n);"
    return output

def visualize_samples(samples, target_num):
    if samples.shape[0] == 0:
        print("没有可视化数据。")
        return
    x_coords, y_coords = samples[:, 0], samples[:, 1]
    fig, ax = plt.subplots(figsize=(8, 8))
    circle_bg = plt.Circle((0, 0), 1, color='lightblue', alpha=0.5, zorder=0)
    ax.add_artist(circle_bg)
    ax.scatter(x_coords, y_coords, s=25, zorder=1, label=f'Generated: {len(x_coords)}')
    ax.set_title(f'Poisson Disk Sampling (Target: {target_num})', fontsize=16)
    ax.set_xlabel('X')
    ax.set_ylabel('Y')
    ax.set_xlim(-1.1, 1.1)
    ax.set_ylim(-1.1, 1.1)
    ax.set_aspect('equal', adjustable='box')
    ax.grid(True, linestyle='--', alpha=0.6)
    ax.legend()
    plt.show()

# --------------------------------------------------------------------------
if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="在单位圆内生成泊松盘采样点并输出 HLSL/GLSL 数组。",
        formatter_class=argparse.RawTextHelpFormatter
    )
    parser.add_argument(
        "num_samples", type=int, nargs="?", default=16,
        help="目标采样点数量 (默认 16)"
    )
    parser.add_argument(
        "-l", "--lang", type=str, default="hlsl", choices=["hlsl", "glsl"],
        help="输出着色器语言格式 (默认 hlsl)"
    )
    args = parser.parse_args()
    
    NUM_SAMPLES_TARGET = args.num_samples
    SHADER_LANGUAGE = args.lang

    if NUM_SAMPLES_TARGET <= 0:
        print("采样点数量必须大于0。")
        sys.exit()

    dims = np.array([2.0, 2.0])  # 正方形区域，后面裁剪到单位圆
    area = dims[0] * dims[1]

    # 初始半径估算
    radius = np.sqrt(area / (NUM_SAMPLES_TARGET * 1.2))  # 调大以抵消裁剪损失
    best_samples = np.empty((0,2))
    best_diff = float('inf')
    max_iterations = 15

    print(f"目标采样点数量: {NUM_SAMPLES_TARGET}，正在自动调整半径...")

    for i in range(max_iterations):
        # 生成点
        raw_samples = Bridson_sampling(dims=dims, radius=radius)
        # 平移到中心 (0,0)
        transformed_samples = raw_samples - 1.0
        # 裁剪到单位圆
        final_samples = transformed_samples[np.sum(transformed_samples**2, axis=1) <= 1.0]
        num_found = final_samples.shape[0]
        diff = abs(num_found - NUM_SAMPLES_TARGET)
        print(f"  第 {i+1}/{max_iterations} 次尝试: radius={radius:.4f}, 找到 {num_found} 个点")

        if diff < best_diff:
            best_diff = diff
            best_samples = final_samples
            if diff == 0:
                print("  完全匹配目标数量，提前结束调整。")
                break

        # 根据结果调整半径
        if num_found < NUM_SAMPLES_TARGET:
            radius *= 0.95
        else:
            radius *= 1.05

    final_samples = best_samples
    print("-" * 40)
    print(f"最终生成采样点数量: {final_samples.shape[0]}")
    print("-" * 40)

    # 输出着色器数组
    shader_code = format_for_shader(final_samples, SHADER_LANGUAGE)
    print(f"用于 {SHADER_LANGUAGE.upper()} 的预计算数组：")
    print(shader_code)
    print("-" * 40)

    # 可视化
    print("正在生成可视化图表...")
    visualize_samples(final_samples, NUM_SAMPLES_TARGET)
