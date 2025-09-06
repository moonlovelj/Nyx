import sys
import imageio.v2 as imageio
import numpy as np
import cv2
from skimage.metrics import structural_similarity as ssim

def compare_images(image_a_path, image_b_path, mode="gamma"):
    """
    读取两张HDR图片，计算并可视化它们的差异。

    Args:
        image_a_path (str): 第一张图片的路径。
        image_b_path (str): 第二张图片的路径。
        mode (str): 差异增强模式，可选值 ["normal", "gamma", "log"]
    """
    try:
        # 读取图片
        image_a = imageio.imread(image_a_path, format='HDR-FI' if image_a_path.lower().endswith('.hdr') else 'EXR-FI')
        image_b = imageio.imread(image_b_path, format='HDR-FI' if image_b_path.lower().endswith('.hdr') else 'EXR-FI')
    except FileNotFoundError as e:
        print(f"错误: 文件未找到 - {e}")
        return
    except Exception as e:
        print(f"读取图片时发生错误: {e}")
        return

    # 确保两张图片的尺寸相同
    if image_a.shape != image_b.shape:
        print(f"错误: 两张图片的尺寸不匹配。")
        print(f"  - 图片A尺寸: {image_a.shape}")
        print(f"  - 图片B尺寸: {image_b.shape}")
        return

    # --- 1. 像素值差异计算 ---
    diff = np.abs(image_a - image_b)

    mae = np.mean(diff)  # 平均绝对误差
    mse = np.mean(diff ** 2)  # 均方误差
    
    max_pixel_value = np.max([image_a.max(), image_b.max()])
    if mse == 0:
        psnr = float('inf')
    else:
        psnr = 20 * np.log10(max_pixel_value / np.sqrt(mse))

    print("\n--- 像素值差异分析 ---")
    print(f"平均绝对误差 (MAE): {mae:.6f}")
    print(f"均方误差 (MSE): {mse:.6f}")
    print(f"峰值信噪比 (PSNR): {psnr:.2f} dB")

    # --- 2. 视觉差异计算 ---
    data_range = max_pixel_value - np.min([image_a.min(), image_b.min()])
    ssim_index = ssim(image_a, image_b, channel_axis=-1, data_range=data_range)

    print("\n--- 视觉差异分析 ---")
    print(f"结构相似性 (SSIM): {ssim_index:.6f} (值越接近1, 表示视觉上越相似)")

    # --- 3. 差异可视化 ---
    diff_gray = np.mean(diff, axis=2)

    # 采用 99% 分位数作为归一化上限，避免极端值影响
    vmax = np.percentile(diff_gray, 99)
    diff_norm = np.clip(diff_gray / (vmax + 1e-8), 0, 1)

    if mode == "gamma":
        gamma = 0.5  # 小于1让小差异更明显
        diff_proc = np.power(diff_norm, gamma)
    elif mode == "log":
        diff_proc = np.log1p(diff_norm * 100) / np.log(101)  # log映射
    else:  # normal
        diff_proc = diff_norm

    # 转换到 0-255
    diff_visual = (diff_proc * 255).astype(np.uint8)

    # 伪彩色映射
    diff_heatmap = cv2.applyColorMap(diff_visual, cv2.COLORMAP_TURBO)

    output_filename = f'difference_visualization_{mode}.png'
    try:
        cv2.imwrite(output_filename, diff_heatmap)
        print(f"\n差异可视化图片已保存为: {output_filename}")
    except Exception as e:
        print(f"保存可视化图片时发生错误: {e}")


if __name__ == "__main__":
    if len(sys.argv) < 3 or len(sys.argv) > 4:
        print("使用方法: python compare_hdr.py <图片A路径> <图片B路径> [模式: normal|gamma|log]")
        sys.exit(1)

    image_path_a = sys.argv[1]
    image_path_b = sys.argv[2]
    mode = sys.argv[3] if len(sys.argv) == 4 else "gamma"

    compare_images(image_path_a, image_path_b, mode)
