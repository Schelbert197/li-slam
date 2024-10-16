from setuptools import find_packages, setup

package_name = 'frontier_exp'

setup(
    name=package_name,
    version='0.0.0',
    packages=find_packages(exclude=['test']),
    data_files=[
        ('share/ament_index/resource_index/packages',
            ['resource/' + package_name]),
        ('share/' + package_name, ['package.xml',
                                   'launch/frontier_nav2_lc.launch.py']),
    ],
    install_requires=['setuptools'],
    zip_safe=True,
    maintainer='sags',
    maintainer_email='srikanthschelbert2024@u.northwestern.edu',
    description='A package to publish goal frontiers for frontier exploration with Nav2',
    license='Apache-2.0',
    tests_require=['pytest'],
    entry_points={
        'console_scripts': [
            'frontier_lc = frontier_exp.frontier_lifecycle:main'
        ],
    },
)
