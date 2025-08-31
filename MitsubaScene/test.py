import mitsuba as mi
# print(mi.variants())
mi.set_variant("cuda_ad_rgb")
scene = mi.load_file("env/env.xml")
image = mi.render(scene, spp=256)
import matplotlib.pyplot as plt

plt.axis("off")
plt.imshow(image ** (1.0 / 2.2)); # approximate sRGB tonemapping
mi.util.write_bitmap("env.png", image)
mi.util.write_bitmap("env.exr", image)