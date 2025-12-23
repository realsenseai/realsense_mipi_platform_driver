import org.jenkinsci.plugins.workflow.steps.FlowInterruptedException

pipeline {
	agent { label 'rs-orin-01.realsenseai.com' }

	options {
		timestamps()
		timeout(time: 30, unit: 'MINUTES')
	}

	stages {
		stage('Get artifacts') {
			steps {
				script {
					copyArtifacts filter: '**/*.tar.bz2',
						projectName: 'D4xx_Kernel_Module_Jetson_JP6',
						flatten: true,
						target: 'artifacts/'
				}
			}
		}
		stage('Install artifacts') {
			steps {
				script {
					sh """#!/bin/sh
						tar -xf 'artifacts/rootfs.tar.bz2'
						"""
				}
			}
		}
		stage('Pytest') {
			steps {
				script {
					sh 'pytest --tb=no -s test'
				}
			}
		}
	}
}
