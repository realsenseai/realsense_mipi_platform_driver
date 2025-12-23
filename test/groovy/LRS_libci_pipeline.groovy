import org.jenkinsci.plugins.workflow.steps.FlowInterruptedException

pipeline {
	agent { label 'rs-orin-01.realsenseai.com' }

	options {
		timestamps()
			timeout(time: 30, unit: 'MINUTES')
	}

	parameters {
		booleanParam(name: 'REBOOTING', defaultValue: false)
	}

	stages {
		stage('Get artifacts') {
			when {
				expression { params.REBOOTING == false }
			}
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
			when {
				expression { params.REBOOTING == false }
			}
			steps {
				sh """#!/bin/sh
					tar -xf artifacts/rootfs.tar.bz2
					sudo cp -R lib/modules/* /lib/modules
					sudo cp -R boot/* /boot/
					touch rebooting
				"""
				script {
					build job: env.JOB_NAME, parameters: [
						string(name: 'REBOOTING', value: true)
						]
				}
				sh 'sudo reboot'
			}
		}
		stage('Pytest') {
			when {
				expression { params.REBOOTING == true }
			}
			steps {
				sh """#!/bin/sh
					rm rebooting
					pytest --tb=no -s test'
				"""
			}
		}
	}
}
