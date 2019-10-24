import matplotlib.pyplot as plt
import numpy as np


font = {'family' : 'normal',
        # 'weight' : 'bold',
        'size'   : 18}
font_label = {'family' : 'normal',
        # 'weight' : 'bold',
        'size'   : 18}

plt.rc('font', **font)

x = np.arange(6)
y = [11, 18, 23, 35, 48]
plt.bar(x, y)

plt.xticks(x, ('0%', '1%', '2%', '5%', '10%', '20%'))
axs.set_ylabel('Transfer Time (seconds)')
axs.set_ylabel('Loss Rate')
axs.grid('on',axis='y')

plt.savefig('ds2.png', format='png', dpi=300, bbox_inches='tight')
plt.show()